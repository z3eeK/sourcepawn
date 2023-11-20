// vim: set ts=8 sts=4 sw=4 tw=99 et:
//  Pawn compiler - Recursive descend expresion parser
//
//  Copyright (c) ITB CompuPhase, 1997-2005
//  Copyright (c) AlliedModders LLC 2021
//
//  This software is provided "as-is", without any express or implied warranty.
//  In no event will the authors be held liable for any damages arising from
//  the use of this software.
//
//  Permission is granted to anyone to use this software for any purpose,
//  including commercial applications, and to alter it and redistribute it
//  freely, subject to the following restrictions:
//
//  1.  The origin of this software must not be misrepresented; you must not
//      claim that you wrote the original software. If you use this software in
//      a product, an acknowledgment in the product documentation would be
//      appreciated but is not required.
//  2.  Altered source versions must be plainly marked as such, and must not be
//      misrepresented as being the original software.
//  3.  This notice may not be removed or altered from any source distribution.
//
#include "symbols.h"

#include "array-helpers.h"
#include "compile-context.h"
#include "errors.h"
#include "lexer.h"
#include "parser.h"
#include "sc.h"
#include "semantics.h"

namespace sp {

void AddGlobal(CompileContext& cc, symbol* sym)
{
    assert(sym->vclass() == sGLOBAL);

    auto scope = cc.globals();
    scope->AddChain(sym->decl());
}

void markusage(Decl* decl, int usage) {
    if (auto var = decl->as<VarDeclBase>()) {
        if (usage & uREAD)
            var->set_is_read();
        if (usage & uWRITTEN)
            var->set_is_written();
        return;
    }

    auto& cc = CompileContext::get();
    if (!cc.sema())
        return;

    auto parent_func = cc.sema()->func_node();
    if (!parent_func)
        return;

    auto fun = decl->as<FunctionDecl>();
    if (!fun)
        return;
    fun = fun->canonical();

    symbol* sym = fun->sym();

    // The reference graph only contains outgoing edges to global or file-static
    // variables. Locals and such are computed by TestSymbols and don't need
    // special handling, there's no concept of "stock" there.
    if (sym->vclass() != sGLOBAL && sym->vclass() != sSTATIC)
        return;
    if (sym->ident != iFUNCTN)
        return;

    assert(parent_func->canonical() == parent_func);
    parent_func->AddReferenceTo(decl->as<FunctionDecl>()->canonical());
}

void markusage(symbol* sym, int usage) {
    markusage(sym->decl(), usage);
}

symbol::symbol(Decl* decl, cell symaddr, IdentifierKind symident, int symvclass, int symtag)
 : vclass_((char)symvclass),
   tag_(symtag),
   ident(symident),
   is_const_(false),
   semantic_tag_(0),
   dim_data_(nullptr),
   decl_(decl),
   addr_(symaddr)
{
    assert(ident != iINVALID);
    assert(decl);
}

void symbol::set_dim_count(int dim_count) {
    if (this->dim_count() == dim_count)
        return;

    auto& cc = CompileContext::get();
    dim_data_ = cc.allocator().alloc<int>(dim_count + 1);
    dim_data_[0] = dim_count;
    dim_data_++;
}

symbol*
NewVariable(Decl* decl, cell addr, IdentifierKind ident, int vclass, int tag, int dim[],
            int numdim, int semantic_tag)
{
    symbol* sym = new symbol(decl, addr, ident, vclass, tag);

    if (numdim) {
        sym->set_dim_count(numdim);
        for (int i = 0; i < numdim; i++)
            sym->set_dim(i, dim[i]);
        sym->set_semantic_tag(semantic_tag);
    }
    return sym;
}

Decl* FindEnumStructField(Type* type, Atom* name) {
    auto decl = type->asEnumStruct();
    if (!decl)
        return nullptr;

    for (const auto& field : decl->fields()) {
        if (field->name() == name)
            return field;
    }
    for (const auto& method : decl->methods()) {
        if (method->decl_name() == name)
            return method;
    }
    return nullptr;
}

int
check_operatortag(int opertok, int resulttag, const char* opername)
{
    assert(opername != NULL && strlen(opername) > 0);
    switch (opertok) {
        case '!':
        case '<':
        case '>':
        case tlEQ:
        case tlNE:
        case tlLE:
        case tlGE:
            if (resulttag != CompileContext::get().types()->tag_bool()) {
                report(63) << opername << "bool:"; /* operator X requires a "bool:" result tag */
                return FALSE;
            }
            break;
        case '~':
            if (resulttag != 0) {
                report(63) << opername << "_:"; /* operator "~" requires a "_:" result tag */
                return FALSE;
            }
            break;
    }
    return TRUE;
}

enum class NewNameStatus {
    Ok,
    Shadowed,
    Duplicated
};

static NewNameStatus GetNewNameStatus(SemaContext& sc, Atom* name, int vclass) {
    SymbolScope* scope;
    Decl* decl = nullptr;
    if (sc.func_node() && sc.func_node()->is_native()) {
        decl = sc.scope()->Find(name);
        scope = sc.scope();
    } else {
        decl = FindSymbol(sc, name, &scope);
    }
    if (!decl)
        return NewNameStatus::Ok;

    SymbolScope* current = sc.ScopeForAdd();
    if (scope->kind() == sGLOBAL && current->IsGlobalOrFileStatic()) {
        if (vclass == sSTATIC)
            return NewNameStatus::Shadowed;
        return NewNameStatus::Duplicated;
    }
    if (scope == current)
        return NewNameStatus::Duplicated;
    if (current->kind() == sARGUMENT && decl->as<FunctionDecl>())
        return NewNameStatus::Ok;
    return NewNameStatus::Shadowed;
}

bool
CheckNameRedefinition(SemaContext& sc, Atom* name, const token_pos_t& pos, int vclass)
{
    auto name_status = GetNewNameStatus(sc, name, vclass);
    if (name_status == NewNameStatus::Duplicated) {
        report(pos, 21) << name;
        return false;
    }
    if (name_status == NewNameStatus::Shadowed)
        report(pos, 219) << name;
    return true;
}

static symbol*
NewConstant(Decl* decl, const token_pos_t& pos, cell val, int vclass, int tag)
{
    return new symbol(decl, val, iCONSTEXPR, vclass, tag);
}

symbol* DefineConstant(SemaContext& sc, Decl* decl, const token_pos_t& pos, cell val,
                       int vclass, int tag)
{
    auto sym = NewConstant(decl, pos, val, vclass, tag);
    if (CheckNameRedefinition(sc, decl->name(), pos, vclass))
        DefineSymbol(sc, decl, vclass);
    return sym;
}

Decl* FindSymbol(SymbolScope* scope, Atom* name, SymbolScope** found) {
    for (auto iter = scope; iter; iter = iter->parent()) {
        if (auto decl = iter->Find(name)) {
            if (found)
                *found = iter;
            return decl;
        }
    }
    return nullptr;
}

Decl* FindSymbol(SemaContext& sc, Atom* name, SymbolScope** found) {
    return FindSymbol(sc.scope(), name, found);
}

void DefineSymbol(SemaContext& sc, Decl* decl, int vclass) {
    auto scope = sc.ScopeForAdd();
    if (scope->kind() == sFILE_STATIC && vclass != sSTATIC) {
        // The default scope is global scope, but "file static" scope comes
        // earlier in the lookup hierarchy, so skip past it if we need to.
        assert(vclass == sGLOBAL);
        assert(scope->parent()->kind() == sGLOBAL);
        scope = scope->parent();
    }
    if (scope->kind() == sGLOBAL || scope->kind() == sFILE_STATIC)
        scope->AddChain(decl);
    else
        scope->Add(decl);
}

} // namespace sp
