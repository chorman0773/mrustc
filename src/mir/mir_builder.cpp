/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * mir/mir_builder.cpp
 * - MIR Building Helper
 */
#include <algorithm>
#include "from_hir.hpp"

// --------------------------------------------------------------------
// MirBuilder
// --------------------------------------------------------------------
MirBuilder::MirBuilder(const Span& sp, const StaticTraitResolve& resolve, const ::HIR::Function::args_t& args, ::MIR::Function& output):
    m_root_span(sp),
    m_resolve(resolve),
    m_args(args),
    m_output(output),
    m_lang_Box(nullptr),
    m_block_active(false),
    m_result_valid(false),
    m_fcn_scope(*this, 0)
{
    if( resolve.m_crate.m_lang_items.count("owned_box") > 0 ) {
        m_lang_Box = &resolve.m_crate.m_lang_items.at("owned_box");
    }

    set_cur_block( new_bb_unlinked() );
    m_scopes.push_back( ScopeDef { sp } );
    m_scope_stack.push_back( 0 );

    m_scopes.push_back( ScopeDef { sp, ScopeType::make_Temporaries({}) } );
    m_scope_stack.push_back( 1 );

    m_variable_states.reserve( output.named_variables.size() );
    for(unsigned int i = 0; i < output.named_variables.size(); i ++ )
        m_variable_states.push_back( VarState::make_Invalid(InvalidType::Uninit) );
}
MirBuilder::~MirBuilder()
{
    const auto& sp = m_root_span;
    if( block_active() )
    {
        if( has_result() )
        {
            push_stmt_assign( sp, ::MIR::LValue::make_Return({}), get_result(sp) );
        }
        terminate_scope( sp, ScopeHandle { *this, 1 } );
        terminate_scope( sp, mv$(m_fcn_scope) );
        end_block( ::MIR::Terminator::make_Return({}) );
    }
}

const ::HIR::TypeRef* MirBuilder::is_type_owned_box(const ::HIR::TypeRef& ty) const
{
    if( m_lang_Box )
    {
        if( ! ty.m_data.is_Path() ) {
            return nullptr;
        }
        const auto& te = ty.m_data.as_Path();

        if( ! te.path.m_data.is_Generic() ) {
            return nullptr;
        }
        const auto& pe = te.path.m_data.as_Generic();

        if( pe.m_path != *m_lang_Box ) {
            return nullptr;
        }
        // TODO: Properly assert?
        return &pe.m_params.m_types.at(0);
    }
    else
    {
        return nullptr;
    }
}

void MirBuilder::define_variable(unsigned int idx)
{
    DEBUG("DEFINE var" << idx  << ": " << m_output.named_variables.at(idx));
    for( auto scope_idx : ::reverse(m_scope_stack) )
    {
        auto& scope_def = m_scopes.at(scope_idx);
        TU_MATCH_DEF( ScopeType, (scope_def.data), (e),
        (
            ),
        (Variables,
            auto it = ::std::find(e.vars.begin(), e.vars.end(), idx);
            assert(it == e.vars.end());
            e.vars.push_back( idx );
            return ;
            ),
        (Split,
            BUG(Span(), "Variable " << idx << " introduced within a Split");
            )
        )
    }
    BUG(Span(), "Variable " << idx << " introduced with no Variable scope");
}
::MIR::LValue MirBuilder::new_temporary(const ::HIR::TypeRef& ty)
{
    unsigned int rv = m_output.temporaries.size();
    DEBUG("DEFINE tmp" << rv << ": " << ty);

    m_output.temporaries.push_back( ty.clone() );
    m_temporary_states.push_back( VarState::make_Invalid(InvalidType::Uninit) );
    assert(m_output.temporaries.size() == m_temporary_states.size());

    ScopeDef* top_scope = nullptr;
    for(unsigned int i = m_scope_stack.size(); i --; )
    {
        auto idx = m_scope_stack[i];
        if( m_scopes.at( idx ).data.is_Temporaries() ) {
            top_scope = &m_scopes.at(idx);
            break ;
        }
    }
    assert( top_scope );
    auto& tmp_scope = top_scope->data.as_Temporaries();
    tmp_scope.temporaries.push_back( rv );
    return ::MIR::LValue::make_Temporary({rv});
}
::MIR::LValue MirBuilder::lvalue_or_temp(const Span& sp, const ::HIR::TypeRef& ty, ::MIR::RValue val)
{
    TU_IFLET(::MIR::RValue, val, Use, e,
        return mv$(e);
    )
    else {
        auto temp = new_temporary(ty);
        push_stmt_assign( sp, ::MIR::LValue(temp.as_Temporary()), mv$(val) );
        return temp;
    }
}

::MIR::RValue MirBuilder::get_result(const Span& sp)
{
    if(!m_result_valid) {
        BUG(sp, "No value avaliable");
    }
    auto rv = mv$(m_result);
    m_result_valid = false;
    return rv;
}

::MIR::LValue MirBuilder::get_result_unwrap_lvalue(const Span& sp)
{
    auto rv = get_result(sp);
    TU_IFLET(::MIR::RValue, rv, Use, e,
        return mv$(e);
    )
    else {
        BUG(sp, "LValue expected, got RValue");
    }
}
::MIR::LValue MirBuilder::get_result_in_lvalue(const Span& sp, const ::HIR::TypeRef& ty)
{
    auto rv = get_result(sp);
    TU_IFLET(::MIR::RValue, rv, Use, e,
        return mv$(e);
    )
    else {
        auto temp = new_temporary(ty);
        push_stmt_assign( sp, ::MIR::LValue(temp.clone()), mv$(rv) );
        return temp;
    }
}
void MirBuilder::set_result(const Span& sp, ::MIR::RValue val)
{
    if(m_result_valid) {
        BUG(sp, "Pushing a result over an existing result");
    }
    m_result = mv$(val);
    m_result_valid = true;
}

void MirBuilder::push_stmt_assign(const Span& sp, ::MIR::LValue dst, ::MIR::RValue val)
{
    DEBUG(dst << " = " << val);
    ASSERT_BUG(sp, m_block_active, "Pushing statement with no active block");
    ASSERT_BUG(sp, dst.tag() != ::MIR::LValue::TAGDEAD, "");
    ASSERT_BUG(sp, val.tag() != ::MIR::RValue::TAGDEAD, "");

    TU_MATCHA( (val), (e),
    (Use,
        this->moved_lvalue(sp, e);
        ),
    (Constant,
        ),
    (SizedArray,
        this->moved_lvalue(sp, e.val);
        ),
    (Borrow,
        if( e.type == ::HIR::BorrowType::Owned ) {
            TODO(sp, "Move using &move");
            // Likely would require a marker that ensures that the memory isn't reused.
            this->moved_lvalue(sp, e.val);
        }
        else {
            // Doesn't move
        }
        ),
    (Cast,
        this->moved_lvalue(sp, e.val);
        ),
    (BinOp,
        switch(e.op)
        {
        case ::MIR::eBinOp::EQ:
        case ::MIR::eBinOp::NE:
        case ::MIR::eBinOp::GT:
        case ::MIR::eBinOp::GE:
        case ::MIR::eBinOp::LT:
        case ::MIR::eBinOp::LE:
            // Takes an implicit borrow... and only works on copy, so why is this block here?
            break;
        default:
            this->moved_lvalue(sp, e.val_l);
            this->moved_lvalue(sp, e.val_r);
            break;
        }
        ),
    (UniOp,
        this->moved_lvalue(sp, e.val);
        ),
    (DstMeta,
        // Doesn't move
        ),
    (DstPtr,
        // Doesn't move
        ),
    (MakeDst,
        // Doesn't move ptr_val
        this->moved_lvalue(sp, e.meta_val);
        ),
    (Tuple,
        for(const auto& val : e.vals)
            this->moved_lvalue(sp, val);
        ),
    (Array,
        for(const auto& val : e.vals)
            this->moved_lvalue(sp, val);
        ),
    (Variant,
        this->moved_lvalue(sp, e.val);
        ),
    (Struct,
        for(const auto& val : e.vals)
            this->moved_lvalue(sp, val);
        )
    )

    // Drop target if populated
    mark_value_assigned(sp, dst);
    m_output.blocks.at(m_current_block).statements.push_back( ::MIR::Statement::make_Assign({ mv$(dst), mv$(val) }) );
}
void MirBuilder::push_stmt_drop(const Span& sp, ::MIR::LValue val, unsigned int flag/*=~0u*/)
{
    ASSERT_BUG(sp, m_block_active, "Pushing statement with no active block");
    ASSERT_BUG(sp, val.tag() != ::MIR::LValue::TAGDEAD, "");

    if( lvalue_is_copy(sp, val) ) {
        // Don't emit a drop for Copy values
        return ;
    }

    DEBUG("DROP " << val);

    auto stmt = ::MIR::Statement::make_Drop({ ::MIR::eDropKind::DEEP, mv$(val), flag });

    m_output.blocks.at(m_current_block).statements.push_back( mv$(stmt) );
}
void MirBuilder::push_stmt_drop_shallow(const Span& sp, ::MIR::LValue val)
{
    ASSERT_BUG(sp, m_block_active, "Pushing statement with no active block");
    ASSERT_BUG(sp, val.tag() != ::MIR::LValue::TAGDEAD, "");

    // TODO: Ensure that the type is a Box
    //if( lvalue_is_copy(sp, val) ) {
    //    // Don't emit a drop for Copy values
    //    return ;
    //}

    DEBUG("DROP shallow " << val);
    m_output.blocks.at(m_current_block).statements.push_back( ::MIR::Statement::make_Drop({ ::MIR::eDropKind::SHALLOW, mv$(val), ~0u }) );
}
void MirBuilder::push_stmt_asm(const Span& sp, ::MIR::Statement::Data_Asm data)
{
    ASSERT_BUG(sp, m_block_active, "Pushing statement with no active block");

    // 1. Mark outputs as valid
    for(const auto& v : data.outputs)
        mark_value_assigned(sp, v.second);

    // 2. Push
    m_output.blocks.at(m_current_block).statements.push_back( ::MIR::Statement::make_Asm( mv$(data) ) );
}
void MirBuilder::push_stmt_set_dropflag(const Span& sp, unsigned int idx, bool value)
{
    ASSERT_BUG(sp, m_block_active, "Pushing statement with no active block");
    m_output.blocks.at(m_current_block).statements.push_back( ::MIR::Statement::make_SetDropFlag({ idx, value }) );
}

void MirBuilder::mark_value_assigned(const Span& sp, const ::MIR::LValue& dst)
{
    VarState*   state_p = nullptr;
    TU_MATCH_DEF(::MIR::LValue, (dst), (e),
    (
        ),
    (Temporary,
        state_p = &get_temp_state_mut(sp, e.idx);
        if( const auto* se = state_p->opt_Invalid() )
        {
            if( *se != InvalidType::Uninit ) {
                BUG(sp, "Reassigning temporary " << e.idx << " - " << *state_p);
            }
        }
        else {
            // TODO: This should be a bug, but some of the match code ends up reassigning so..
            //BUG(sp, "Reassigning temporary " << e.idx << " - " << *state_p);
        }
        ),
    (Return,
        // Don't drop.
        // No state tracking for the return value
        ),
    (Variable,
        // TODO: Ensure that slot is mutable (information is lost, assume true)
        state_p = &get_variable_state_mut(sp, e);
        )
    )

    if( state_p )
    {
        TU_MATCHA( (*state_p), (se),
        (Invalid,
            ASSERT_BUG(sp, se != InvalidType::Descoped, "Assining of descoped variable - " << dst);
            ),
        (Valid,
            push_stmt_drop( sp, dst.clone() );
            ),
        (Optional,
            push_stmt_drop( sp, dst.clone(), se );
            ),
        (Partial,
            // TODO: Check type, if Box emit _shallow, otherwise destructure drop
            push_stmt_drop_shallow( sp, dst.clone() );
            )
        )
        *state_p = VarState::make_Valid({});
    }
}

void MirBuilder::raise_variables(const Span& sp, const ::MIR::LValue& val, const ScopeHandle& scope)
{
    TRACE_FUNCTION_F(val);
    TU_MATCH_DEF(::MIR::LValue, (val), (e),
    (
        ),
    // TODO: This may not be correct, because it can change the drop points and ordering
    // HACK: Working around cases where values are dropped while the result is not yet used.
    (Deref,
        raise_variables(sp, *e.val, scope);
        ),
    (Field,
        raise_variables(sp, *e.val, scope);
        ),
    (Downcast,
        raise_variables(sp, *e.val, scope);
        ),
    // Actual value types
    (Variable,
        auto idx = e;
        auto scope_it = m_scope_stack.rbegin();
        while( scope_it != m_scope_stack.rend() )
        {
            auto& scope_def = m_scopes.at(*scope_it);

            TU_IFLET( ScopeType, scope_def.data, Variables, e,
                auto tmp_it = ::std::find( e.vars.begin(), e.vars.end(), idx );
                if( tmp_it != e.vars.end() )
                {
                    e.vars.erase( tmp_it );
                    DEBUG("Raise variable " << idx << " from " << *scope_it);
                    break ;
                }
            )
            // If the variable was defined above the desired scope (i.e. this didn't find it), return
            if( *scope_it == scope.idx )
                return ;
            ++scope_it;
        }
        if( scope_it == m_scope_stack.rend() )
        {
            // Temporary wasn't defined in a visible scope?
            return ;
        }
        ++scope_it;

        while( scope_it != m_scope_stack.rend() )
        {
            auto& scope_def = m_scopes.at(*scope_it);

            TU_IFLET( ScopeType, scope_def.data, Variables, e,
                e.vars.push_back( idx );
                DEBUG("- to " << *scope_it);
                return ;
            )
            ++scope_it;
        }

        DEBUG("- top");
        ),
    (Temporary,
        auto idx = e.idx;
        auto scope_it = m_scope_stack.rbegin();
        while( scope_it != m_scope_stack.rend() )
        {
            auto& scope_def = m_scopes.at(*scope_it);

            TU_IFLET( ScopeType, scope_def.data, Temporaries, e,
                auto tmp_it = ::std::find( e.temporaries.begin(), e.temporaries.end(), idx );
                if( tmp_it != e.temporaries.end() )
                {
                    e.temporaries.erase( tmp_it );
                    DEBUG("Raise temporary " << idx << " from " << *scope_it);
                    break ;
                }
            )

            // If the temporary was defined above the desired scope (i.e. this didn't find it), return
            if( *scope_it == scope.idx )
                return ;
            ++scope_it;
        }
        if( scope_it == m_scope_stack.rend() )
        {
            // Temporary wasn't defined in a visible scope?
            return ;
        }
        ++scope_it;

        while( scope_it != m_scope_stack.rend() )
        {
            auto& scope_def = m_scopes.at(*scope_it);

            TU_IFLET( ScopeType, scope_def.data, Temporaries, e,
                e.temporaries.push_back( idx );
                DEBUG("- to " << *scope_it);
                return ;
            )
            ++scope_it;
        }

        DEBUG("- top");
        )
    )
}
void MirBuilder::raise_variables(const Span& sp, const ::MIR::RValue& rval, const ScopeHandle& scope)
{
    TU_MATCHA( (rval), (e),
    (Use,
        this->raise_variables(sp, e, scope);
        ),
    (Constant,
        ),
    (SizedArray,
        this->raise_variables(sp, e.val, scope);
        ),
    (Borrow,
        // TODO: Wait, is this valid?
        this->raise_variables(sp, e.val, scope);
        ),
    (Cast,
        this->raise_variables(sp, e.val, scope);
        ),
    (BinOp,
        this->raise_variables(sp, e.val_l, scope);
        this->raise_variables(sp, e.val_r, scope);
        ),
    (UniOp,
        this->raise_variables(sp, e.val, scope);
        ),
    (DstMeta,
        this->raise_variables(sp, e.val, scope);
        ),
    (DstPtr,
        this->raise_variables(sp, e.val, scope);
        ),
    (MakeDst,
        this->raise_variables(sp, e.ptr_val, scope);
        this->raise_variables(sp, e.meta_val, scope);
        ),
    (Tuple,
        for(const auto& val : e.vals)
            this->raise_variables(sp, val, scope);
        ),
    (Array,
        for(const auto& val : e.vals)
            this->raise_variables(sp, val, scope);
        ),
    (Variant,
        this->raise_variables(sp, e.val, scope);
        ),
    (Struct,
        for(const auto& val : e.vals)
            this->raise_variables(sp, val, scope);
        )
    )
}

void MirBuilder::set_cur_block(unsigned int new_block)
{
    ASSERT_BUG(Span(), !m_block_active, "Updating block when previous is active");
    ASSERT_BUG(Span(), new_block < m_output.blocks.size(), "Invalid block ID being started - " << new_block);
    ASSERT_BUG(Span(), m_output.blocks[new_block].terminator.is_Incomplete(), "Attempting to resume a completed block - BB" << new_block);
    DEBUG("BB" << new_block << " START");
    m_current_block = new_block;
    m_block_active = true;
}
void MirBuilder::end_block(::MIR::Terminator term)
{
    if( !m_block_active ) {
        BUG(Span(), "Terminating block when none active");
    }
    DEBUG("BB" << m_current_block << " END -> " << term);
    m_output.blocks.at(m_current_block).terminator = mv$(term);
    m_block_active = false;
    m_current_block = 0;
}
::MIR::BasicBlockId MirBuilder::pause_cur_block()
{
    if( !m_block_active ) {
        BUG(Span(), "Pausing block when none active");
    }
    DEBUG("BB" << m_current_block << " PAUSE");
    m_block_active = false;
    auto rv = m_current_block;
    m_current_block = 0;
    return rv;
}
::MIR::BasicBlockId MirBuilder::new_bb_linked()
{
    auto rv = new_bb_unlinked();
    DEBUG("BB" << rv);
    end_block( ::MIR::Terminator::make_Goto(rv) );
    set_cur_block(rv);
    return rv;
}
::MIR::BasicBlockId MirBuilder::new_bb_unlinked()
{
    auto rv = m_output.blocks.size();
    DEBUG("BB" << rv);
    m_output.blocks.push_back({});
    return rv;
}


unsigned int MirBuilder::new_drop_flag(bool default_state)
{
    auto rv = m_output.drop_flags.size();
    m_output.drop_flags.push_back(default_state);
    DEBUG("(" << default_state << ") = " << rv);
    return rv;
}
unsigned int MirBuilder::new_drop_flag_and_set(const Span& sp, bool set_state)
{
    auto rv = new_drop_flag(!set_state);
    push_stmt_set_dropflag(sp, rv, set_state);
    return rv;
}
bool MirBuilder::get_drop_flag_default(const Span& sp, unsigned int idx)
{
    return m_output.drop_flags.at(idx);
}

ScopeHandle MirBuilder::new_scope_var(const Span& sp)
{
    unsigned int idx = m_scopes.size();
    m_scopes.push_back( ScopeDef {sp, ScopeType::make_Variables({})} );
    m_scope_stack.push_back( idx );
    DEBUG("START (var) scope " << idx);
    return ScopeHandle { *this, idx };
}
ScopeHandle MirBuilder::new_scope_temp(const Span& sp)
{
    unsigned int idx = m_scopes.size();
    m_scopes.push_back( ScopeDef {sp, ScopeType::make_Temporaries({})} );
    m_scope_stack.push_back( idx );
    DEBUG("START (temp) scope " << idx);
    return ScopeHandle { *this, idx };
}
ScopeHandle MirBuilder::new_scope_split(const Span& sp)
{
    unsigned int idx = m_scopes.size();
    m_scopes.push_back( ScopeDef {sp, ScopeType::make_Split({})} );
    m_scopes.back().data.as_Split().arms.push_back( {} );
    m_scope_stack.push_back( idx );
    DEBUG("START (split) scope " << idx);
    return ScopeHandle { *this, idx };
}
ScopeHandle MirBuilder::new_scope_loop(const Span& sp)
{
    unsigned int idx = m_scopes.size();
    m_scopes.push_back( ScopeDef {sp, ScopeType::make_Loop({})} );
    m_scope_stack.push_back( idx );
    DEBUG("START (loop) scope " << idx);
    return ScopeHandle { *this, idx };
}
void MirBuilder::terminate_scope(const Span& sp, ScopeHandle scope, bool emit_cleanup/*=true*/)
{
    TRACE_FUNCTION_F("DONE scope " << scope.idx << " - " << (emit_cleanup ? "CLEANUP" : "NO CLEANUP"));
    // 1. Check that this is the current scope (at the top of the stack)
    if( m_scope_stack.empty() || m_scope_stack.back() != scope.idx )
    {
        DEBUG("- m_scope_stack = [" << m_scope_stack << "]");
        auto it = ::std::find( m_scope_stack.begin(), m_scope_stack.end(), scope.idx );
        if( it == m_scope_stack.end() )
            BUG(sp, "Terminating scope not on the stack - scope " << scope.idx);
        BUG(sp, "Terminating scope " << scope.idx << " when not at top of stack, " << (m_scope_stack.end() - it - 1) << " scopes in the way");
    }

    auto& scope_def = m_scopes.at(scope.idx);
    ASSERT_BUG( sp, scope_def.complete == false, "Terminating scope which is already terminated" );

    if( emit_cleanup )
    {
        // 2. Emit drops for all non-moved variables (share with below)
        drop_scope_values(scope_def);
    }

    // 3. Pop scope (last because `drop_scope_values` uses the stack)
    m_scope_stack.pop_back();

    complete_scope(scope_def);
}
void MirBuilder::terminate_scope_early(const Span& sp, const ScopeHandle& scope, bool loop_exit/*=false*/)
{
    TRACE_FUNCTION_F("EARLY scope " << scope.idx);

    // 1. Ensure that this block is in the stack
    auto it = ::std::find( m_scope_stack.begin(), m_scope_stack.end(), scope.idx );
    if( it == m_scope_stack.end() ) {
        BUG(sp, "Early-terminating scope not on the stack");
    }
    unsigned int slot = it - m_scope_stack.begin();

    bool is_conditional = false;
    for(unsigned int i = m_scope_stack.size(); i-- > slot; )
    {
        auto idx = m_scope_stack[i];
        auto& scope_def = m_scopes.at( idx );

        if( idx == scope.idx )
        {
            // If this is exiting a loop, save the state so the variable state after the loop is known.
            if( loop_exit && scope_def.data.is_Loop() )
            {
                auto& e = scope_def.data.as_Loop();
                SplitArm    sa;
                for(const auto& i : e.changed_vars)
                {
                    sa.var_states.insert( ::std::make_pair(i, get_variable_state(sp, i).clone()) );
                }
                for(const auto& i : e.changed_tmps)
                {
                    sa.tmp_states.insert( ::std::make_pair(i, get_temp_state(sp, i).clone()) );
                }
                e.exit_states.push_back( mv$(sa) );
            }
        }

        // If a conditional block is hit, prevent full termination of the rest
        if( scope_def.data.is_Split() || scope_def.data.is_Loop() )
            is_conditional = true;

        if( !is_conditional ) {
            DEBUG("Complete scope " << idx);
            drop_scope_values(scope_def);
            m_scope_stack.pop_back();
            complete_scope(scope_def);
        }
        else {
            // Mark patial within this scope?
            DEBUG("Drop part of scope " << idx);

            // Emit drops for dropped values within this scope
            drop_scope_values(scope_def);
            // Inform the scope that it's been early-exited
            TU_IFLET( ScopeType, scope_def.data, Split, e,
                e.arms.back().has_early_terminated = true;
            )
        }
    }
}

namespace
{
    static void merge_state(const Span& sp, MirBuilder& builder, const ::MIR::LValue& lv, VarState& old_state, const VarState& new_state)
    {
        DEBUG(lv << " : " << old_state << " <= " << new_state);
        switch(old_state.tag())
        {
        case VarState::TAGDEAD: throw "";
        case VarState::TAG_Invalid:
            switch( new_state.tag() )
            {
            case VarState::TAGDEAD: throw "";
            case VarState::TAG_Invalid:
                // Invalid->Invalid :: Choose the highest of the invalid types (TODO)
                return ;
            case VarState::TAG_Valid:
                // Allocate a drop flag
                old_state = VarState::make_Optional( builder.new_drop_flag_and_set(sp, true) );
                return ;
            case VarState::TAG_Optional:
                // Was invalid, now optional.
                if( builder.get_drop_flag_default( sp, new_state.as_Optional() ) != false ) {
                    TODO(sp, "Drop flag default not false when going Invalid->Optional");
                }
                old_state = VarState::make_Optional( new_state.as_Optional() );
                return ;
            case VarState::TAG_Partial:
                TODO(sp, "Handle Invalid->Partial in split scope");
            }
            break;
        case VarState::TAG_Valid:
            switch( new_state.tag() )
            {
            case VarState::TAGDEAD: throw "";
            case VarState::TAG_Invalid:
                old_state = VarState::make_Optional( builder.new_drop_flag_and_set(sp, false) );
                return ;
            case VarState::TAG_Valid:
                return ;
            case VarState::TAG_Optional:
                // Was valid, now optional.
                if( builder.get_drop_flag_default( sp, new_state.as_Optional() ) != true ) {
                    TODO(sp, "Drop flag default not true when going Valid->Optional");
                }
                old_state = VarState::make_Optional( new_state.as_Optional() );
                return ;
            case VarState::TAG_Partial:
                TODO(sp, "Handle Valid->Partial in split scope");
            }
            break;
        case VarState::TAG_Optional:
            switch( new_state.tag() )
            {
            case VarState::TAGDEAD: throw "";
            case VarState::TAG_Invalid:
                builder.push_stmt_set_dropflag(sp, old_state.as_Optional(), false);
                return ;
            case VarState::TAG_Valid:
                builder.push_stmt_set_dropflag(sp, old_state.as_Optional(), true);
                return ;
            case VarState::TAG_Optional:
                if( old_state.as_Optional() != new_state.as_Optional() ) {
                    TODO(sp, "Handle Optional->Optional with mismatched flags");
                }
                return ;
            case VarState::TAG_Partial:
                TODO(sp, "Handle Optional->Partial in split scope");
            }
            break;
        case VarState::TAG_Partial:
            // Need to tag for conditional shallow drop? Or just do that at the end of the split?
            // - End of the split means that the only optional state is outer drop.
            switch( new_state.tag() )
            {
            case VarState::TAGDEAD: throw "";
            case VarState::TAG_Invalid:
                TODO(sp, "Handle Partial->Invalid in split scope");
            case VarState::TAG_Valid:
                TODO(sp, "Handle Partial->Valid in split scope");
            case VarState::TAG_Optional:
                TODO(sp, "Handle Partial->Optional in split scope");
            case VarState::TAG_Partial:
                ASSERT_BUG(sp, old_state.as_Partial().size() == new_state.as_Partial().size(), "Partial->Partial with mismatchd sizes");
                TODO(sp, "Handle Partial->Partial in split scope");
            }
            break;
        }
        BUG(sp, "Unhandled combination - " << old_state.tag_str() << " and " << new_state.tag_str());
    }
}

void MirBuilder::end_split_arm(const Span& sp, const ScopeHandle& handle, bool reachable)
{
    ASSERT_BUG(sp, handle.idx < m_scopes.size(), "");
    auto& sd = m_scopes.at( handle.idx );
    ASSERT_BUG(sp, sd.data.is_Split(), "");
    auto& sd_split = sd.data.as_Split();
    ASSERT_BUG(sp, !sd_split.arms.empty(), "");

    TRACE_FUNCTION_F("end split scope " << handle.idx << " arm " << (sd_split.arms.size()-1));
    if( reachable )
        ASSERT_BUG(sp, m_block_active, "Block must be active when ending a reachable split arm");

    auto& this_arm_state = sd_split.arms.back();
    this_arm_state.always_early_terminated = /*sd_split.arms.back().has_early_terminated &&*/ !reachable;

    if( sd_split.end_state_valid )
    {
        if( reachable )
        {
            // Insert copies of the parent state 
            for(const auto& ent : this_arm_state.var_states) {
                if( sd_split.end_state.var_states.count(ent.first) == 0 ) {
                    sd_split.end_state.var_states.insert(::std::make_pair( ent.first, get_variable_state(sp, ent.first, 1).clone() ));
                }
            }
            for(const auto& ent : this_arm_state.tmp_states) {
                if( sd_split.end_state.tmp_states.count(ent.first) == 0 ) {
                    sd_split.end_state.tmp_states.insert(::std::make_pair( ent.first, get_temp_state(sp, ent.first, 1).clone() ));
                }
            }

            // Merge state
            for(auto& ent : sd_split.end_state.var_states)
            {
                auto idx = ent.first;
                auto& out_state = ent.second;

                // Merge the states
                auto it = this_arm_state.var_states.find(idx);
                const auto& src_state = (it != this_arm_state.var_states.end() ? it->second : get_variable_state(sp, idx, 1));

                merge_state(sp, *this, ::MIR::LValue::make_Variable(idx), out_state, src_state);
            }
            for(auto& ent : sd_split.end_state.tmp_states)
            {
                auto idx = ent.first;
                auto& out_state = ent.second;

                // Merge the states
                auto it = this_arm_state.tmp_states.find(idx);
                const auto& src_state = (it != this_arm_state.tmp_states.end() ? it->second : get_temp_state(sp, idx, 1));

                merge_state(sp, *this, ::MIR::LValue::make_Temporary({idx}), out_state, src_state);
            }
        }
    }
    else
    {
        // Clone this arm's state
        for(auto& ent : this_arm_state.var_states)
        {
            sd_split.end_state.var_states.insert(::std::make_pair( ent.first, ent.second.clone() ));
        }
        for(auto& ent : this_arm_state.tmp_states)
        {
            sd_split.end_state.tmp_states.insert(::std::make_pair( ent.first, ent.second.clone() ));
        }
        sd_split.end_state_valid = true;
    }

    sd_split.arms.push_back( {} );
}
void MirBuilder::end_split_arm_early(const Span& sp)
{
    TRACE_FUNCTION_F("");
    // Terminate all scopes until a split is found.
    while( ! m_scope_stack.empty() && ! (m_scopes.at( m_scope_stack.back() ).data.is_Split() || m_scopes.at( m_scope_stack.back() ).data.is_Loop()) )
    {
        auto& scope_def = m_scopes[m_scope_stack.back()];
        // Fully drop the scope
        DEBUG("Complete scope " << m_scope_stack.back());
        drop_scope_values(scope_def);
        m_scope_stack.pop_back();
        complete_scope(scope_def);
    }

    if( !m_scope_stack.empty() && m_scopes.at( m_scope_stack.back() ).data.is_Split() )
    {
        DEBUG("Early terminate split scope " << m_scope_stack.back());
        auto& sd = m_scopes[ m_scope_stack.back() ];
        auto& sd_split = sd.data.as_Split();
        sd_split.arms.back().has_early_terminated = true;

        // TODO: Create drop flags if required?
    }
}
void MirBuilder::complete_scope(ScopeDef& sd)
{
    sd.complete = true;

    TU_MATCHA( (sd.data), (e),
    (Temporaries,
        DEBUG("Temporaries " << e.temporaries);
        ),
    (Variables,
        DEBUG("Variables " << e.vars);
        ),
    (Loop,
        DEBUG("Loop");
        ),
    (Split,
        )
    )
    // No macro for better debug output.
    if( sd.data.is_Loop() )
    {
        #if 0
        auto& e = sd.data.as_Loop();
        TRACE_FUNCTION_F("Loop - " << e.exit_states.size() << " breaks");

        // Merge all exit states and apply to output
        H::apply_split_arms(*this, sd.span, e.exit_states);
        #endif
    }
    else if( sd.data.is_Split() )
    {
        auto& e = sd.data.as_Split();
        TRACE_FUNCTION_F("Split - " << (e.arms.size() - 1) << " arms");

        ASSERT_BUG(sd.span, e.end_state_valid, "");
        for(auto& ent : e.end_state.var_states)
        {
            auto& vs = get_variable_state_mut(sd.span, ent.first);
            if( vs != ent.second )
            {
                DEBUG(::MIR::LValue::make_Variable(ent.first) << " " << vs << " => " << ent.second);
                vs = ::std::move(ent.second);
            }
        }
        for(auto& ent : e.end_state.tmp_states)
        {
            auto& vs = get_temp_state_mut(sd.span, ent.first);
            if( vs != ent.second )
            {
                DEBUG(::MIR::LValue::make_Temporary({ent.first}) << " " << vs << " => " << ent.second);
                vs = ::std::move(ent.second);
            }
        }
    }
}

void MirBuilder::with_val_type(const Span& sp, const ::MIR::LValue& val, ::std::function<void(const ::HIR::TypeRef&)> cb) const
{
    TU_MATCH(::MIR::LValue, (val), (e),
    (Variable,
        cb( m_output.named_variables.at(e) );
        ),
    (Temporary,
        cb( m_output.temporaries.at(e.idx) );
        ),
    (Argument,
        ASSERT_BUG(sp, e.idx < m_args.size(), "Argument number out of range");
        cb( m_args.at(e.idx).second );
        ),
    (Static,
        TU_MATCHA( (e.m_data), (pe),
        (Generic,
            ASSERT_BUG(sp, pe.m_params.m_types.empty(), "Path params on static");
            const auto& s = m_resolve.m_crate.get_static_by_path(sp, pe.m_path);
            cb( s.m_type );
            ),
        (UfcsKnown,
            TODO(sp, "Static - UfcsKnown - " << e);
            ),
        (UfcsUnknown,
            BUG(sp, "Encountered UfcsUnknown in Static - " << e);
            ),
        (UfcsInherent,
            TODO(sp, "Static - UfcsInherent - " << e);
            )
        )
        ),
    (Return,
        TODO(sp, "Return");
        ),
    (Field,
        with_val_type(sp, *e.val, [&](const auto& ty){
            TU_MATCH_DEF( ::HIR::TypeRef::Data, (ty.m_data), (te),
            (
                BUG(sp, "Field access on unexpected type - " << ty);
                ),
            (Array,
                cb( *te.inner );
                ),
            (Slice,
                cb( *te.inner );
                ),
            (Path,
                ASSERT_BUG(sp, te.binding.is_Struct(), "Field on non-Struct - " << ty);
                const auto& str = *te.binding.as_Struct();
                TU_MATCHA( (str.m_data), (se),
                (Unit,
                    BUG(sp, "Field on unit-like struct - " << ty);
                    ),
                (Tuple,
                    ASSERT_BUG(sp, e.field_index < se.size(),
                        "Field index out of range in tuple-struct " << ty << " - " << e.field_index << " > " << se.size());
                    const auto& fld = se[e.field_index];
                    if( monomorphise_type_needed(fld.ent) ) {
                        auto sty = monomorphise_type(sp, str.m_params, te.path.m_data.as_Generic().m_params, fld.ent);
                        m_resolve.expand_associated_types(sp, sty);
                        cb(sty);
                    }
                    else {
                        cb(fld.ent);
                    }
                    ),
                (Named,
                    ASSERT_BUG(sp, e.field_index < se.size(),
                        "Field index out of range in struct " << ty << " - " << e.field_index << " > " << se.size());
                    const auto& fld = se[e.field_index].second;
                    if( monomorphise_type_needed(fld.ent) ) {
                        auto sty = monomorphise_type(sp, str.m_params, te.path.m_data.as_Generic().m_params, fld.ent);
                        m_resolve.expand_associated_types(sp, sty);
                        cb(sty);
                    }
                    else {
                        cb(fld.ent);
                    }
                    )
                )
                ),
            (Tuple,
                ASSERT_BUG(sp, e.field_index < te.size(), "Field index out of range in tuple " << e.field_index << " >= " << te.size());
                cb( te[e.field_index] );
                )
            )
            });
        ),
    (Deref,
        with_val_type(sp, *e.val, [&](const auto& ty){
            TU_MATCH_DEF( ::HIR::TypeRef::Data, (ty.m_data), (te),
            (
                BUG(sp, "Deref on unexpected type - " << ty);
                ),
            (Path,
                if( const auto* inner_ptr = this->is_type_owned_box(ty) )
                {
                    cb( *inner_ptr );
                }
                else {
                    BUG(sp, "Deref on unexpected type - " << ty);
                }
                ),
            (Pointer,
                cb(*te.inner);
                ),
            (Borrow,
                cb(*te.inner);
                )
            )
            });
        ),
    (Index,
        with_val_type(sp, *e.val, [&](const auto& ty){
            TU_MATCH_DEF( ::HIR::TypeRef::Data, (ty.m_data), (te),
            (
                BUG(sp, "Index on unexpected type - " << ty);
                ),
            (Slice,
                cb(*te.inner);
                ),
            (Array,
                cb(*te.inner);
                )
            )
            });
        ),
    (Downcast,
        with_val_type(sp, *e.val, [&](const auto& ty){
            TU_MATCH_DEF( ::HIR::TypeRef::Data, (ty.m_data), (te),
            (
                BUG(sp, "Downcast on unexpected type - " << ty);
                ),
            (Path,
                //ASSERT_BUG(sp, !te.binding.is_Unbound(), "Unbound path " << ty << " encountered");
                ASSERT_BUG(sp, te.binding.is_Enum(), "Downcast on non-Enum - " << ty << " for " << val);
                const auto& enm = *te.binding.as_Enum();
                const auto& variants = enm.m_variants;
                ASSERT_BUG(sp, e.variant_index < variants.size(), "Variant index out of range");
                const auto& variant = variants[e.variant_index];
                // TODO: Make data variants refer to associated types (unify enum and struct handling)
                TU_MATCHA( (variant.second), (ve),
                (Value,
                    ),
                (Unit,
                    ),
                (Tuple,
                    // HACK! Create tuple.
                    ::std::vector< ::HIR::TypeRef>  tys;
                    for(const auto& fld : ve)
                        tys.push_back( monomorphise_type(sp, enm.m_params, te.path.m_data.as_Generic().m_params, fld.ent) );
                    ::HIR::TypeRef  tup( mv$(tys) );
                    m_resolve.expand_associated_types(sp, tup);
                    cb(tup);
                    ),
                (Struct,
                    // HACK! Create tuple.
                    ::std::vector< ::HIR::TypeRef>  tys;
                    for(const auto& fld : ve)
                        tys.push_back( monomorphise_type(sp, enm.m_params, te.path.m_data.as_Generic().m_params, fld.second.ent) );
                    ::HIR::TypeRef  tup( mv$(tys) );
                    m_resolve.expand_associated_types(sp, tup);
                    cb(tup);
                    )
                )
                )
            )
            });
        )
    )
}

bool MirBuilder::lvalue_is_copy(const Span& sp, const ::MIR::LValue& val) const
{
    int rv = 0;
    with_val_type(sp, val, [&](const auto& ty){
        DEBUG("[lvalue_is_copy] ty="<<ty);
        rv = (m_resolve.type_is_copy(sp, ty) ? 2 : 1);
        });
    assert(rv != 0);
    return rv == 2;
}

const VarState& MirBuilder::get_variable_state(const Span& sp, unsigned int idx, unsigned int skip_count) const
{
    for( auto scope_idx : ::reverse(m_scope_stack) )
    {
        const auto& scope_def = m_scopes.at(scope_idx);
        TU_MATCH_DEF( ScopeType, (scope_def.data), (e),
        (
            ),
        (Variables,
            auto it = ::std::find(e.vars.begin(), e.vars.end(), idx);
            if( it != e.vars.end() ) {
                // If controlled by this block, exit early (won't find it elsewhere)
                break ;
            }
            ),
        (Split,
            const auto& cur_arm = e.arms.back();
            auto it = cur_arm.var_states.find(idx);
            if( it != cur_arm.var_states.end() )
            {
                if( ! skip_count -- )
                {
                    return it->second;
                }
            }
            )
        )
    }

    ASSERT_BUG(sp, idx < m_variable_states.size(), "Variable " << idx << " out of range for state table");
    return m_variable_states[idx];
}
VarState& MirBuilder::get_variable_state_mut(const Span& sp, unsigned int idx)
{
    for( auto scope_idx : ::reverse(m_scope_stack) )
    {
        auto& scope_def = m_scopes.at(scope_idx);
        if( scope_def.data.is_Variables() )
        {
            const auto& e = scope_def.data.as_Variables();
            auto it = ::std::find(e.vars.begin(), e.vars.end(), idx);
            if( it != e.vars.end() ) {
                break ;
            }
        }
        else if( scope_def.data.is_Split() )
        {
            auto& e = scope_def.data.as_Split();
            auto& cur_arm = e.arms.back();
            auto it = cur_arm.var_states.find(idx);
            if( it == cur_arm.var_states.end() )
                return cur_arm.var_states[idx] = get_variable_state(sp, idx).clone();
            return it->second;
        }
        else if( scope_def.data.is_Loop() )
        {
            auto& e = scope_def.data.as_Loop();
            e.changed_vars.insert( idx );
        }
        else
        {
        }
    }

    ASSERT_BUG(sp, idx < m_variable_states.size(), "Variable " << idx << " out of range for state table");
    return m_variable_states[idx];
}
const VarState& MirBuilder::get_temp_state(const Span& sp, unsigned int idx, unsigned int skip_count) const
{
    for( auto scope_idx : ::reverse(m_scope_stack) )
    {
        const auto& scope_def = m_scopes.at(scope_idx);
        if( scope_def.data.is_Temporaries() )
        {
            const auto& e = scope_def.data.as_Temporaries();
            auto it = ::std::find(e.temporaries.begin(), e.temporaries.end(), idx);
            if( it != e.temporaries.end() ) {
                break ;
            }
        }
        else if( scope_def.data.is_Split() )
        {
            const auto& e = scope_def.data.as_Split();
            const auto& cur_arm = e.arms.back();
            auto it = cur_arm.tmp_states.find(idx);
            if( it != cur_arm.tmp_states.end() )
            {
                if( ! skip_count -- )
                {
                    return it->second;
                }
            }
        }
    }

    ASSERT_BUG(sp, idx < m_temporary_states.size(), "Temporary " << idx << " out of range for state table");
    return m_temporary_states[idx];
}
VarState& MirBuilder::get_temp_state_mut(const Span& sp, unsigned int idx)
{
    for( auto scope_idx : ::reverse(m_scope_stack) )
    {
        auto& scope_def = m_scopes.at(scope_idx);
        if( scope_def.data.is_Temporaries() )
        {
            const auto& e = scope_def.data.as_Temporaries();
            auto it = ::std::find(e.temporaries.begin(), e.temporaries.end(), idx);
            if( it != e.temporaries.end() ) {
                break ;
            }
        }
        else if( scope_def.data.is_Split() )
        {
            auto& e = scope_def.data.as_Split();
            auto& cur_arm = e.arms.back();
            auto it = cur_arm.tmp_states.find(idx);
            if(it == cur_arm.tmp_states.end())
                return cur_arm.tmp_states[idx] = get_temp_state(sp, idx).clone();
            return it->second;
        }
        else if( scope_def.data.is_Loop() )
        {
            auto& e = scope_def.data.as_Loop();
            e.changed_tmps.insert( idx );
        }
        else
        {
        }
    }

    ASSERT_BUG(sp, idx < m_temporary_states.size(), "Temporary " << idx << " out of range for state table");
    return m_temporary_states[idx];
}

void MirBuilder::drop_scope_values(const ScopeDef& sd)
{
    TU_MATCHA( (sd.data), (e),
    (Temporaries,
        for(auto tmp_idx : ::reverse(e.temporaries))
        {
            const auto& vs = get_temp_state(sd.span, tmp_idx);
            TU_MATCHA( (vs), (vse),
            (Invalid,
                ),
            (Valid,
                push_stmt_drop( sd.span, ::MIR::LValue::make_Temporary({ tmp_idx }) );
                ),
            (Partial,
                // TODO: Actual destructuring
                push_stmt_drop_shallow( sd.span, ::MIR::LValue::make_Temporary({ tmp_idx }) );
                ),
            (Optional,
                push_stmt_drop(sd.span, ::MIR::LValue::make_Temporary({ tmp_idx }), vse);
                )
            )
        }
        ),
    (Variables,
        for(auto var_idx : ::reverse(e.vars))
        {
            const auto& vs = get_variable_state(sd.span, var_idx);
            TU_MATCHA( (vs), (vse),
            (Invalid,
                ),
            (Valid,
                push_stmt_drop( sd.span, ::MIR::LValue::make_Variable(var_idx) );
                ),
            (Partial,
                // TODO: Actual destructuring
                push_stmt_drop_shallow( sd.span, ::MIR::LValue::make_Variable(var_idx) );
                ),
            (Optional,
                push_stmt_drop(sd.span, ::MIR::LValue::make_Variable(var_idx), vse);
                )
            )
        }
        ),
    (Split,
        // No values, controls parent
        ),
    (Loop,
        // No values
        )
    )
}

void MirBuilder::moved_lvalue(const Span& sp, const ::MIR::LValue& lv)
{
    TRACE_FUNCTION_F(lv);
    TU_MATCHA( (lv), (e),
    (Variable,
        if( !lvalue_is_copy(sp, lv) ) {
            get_variable_state_mut(sp, e) = VarState::make_Invalid(InvalidType::Moved);
        }
        ),
    (Temporary,
        if( !lvalue_is_copy(sp, lv) ) {
            get_temp_state_mut(sp, e.idx) = VarState::make_Invalid(InvalidType::Moved);
        }
        ),
    (Argument,
        //TODO(sp, "Mark argument as moved");
        ),
    (Static,
        //TODO(sp, "Static - Assert that type is Copy");
        ),
    (Return,
        BUG(sp, "Read of return value");
        ),
    (Field,
        if( lvalue_is_copy(sp, lv) ) {
        }
        else {
            // TODO: Partial moves.
            moved_lvalue(sp, *e.val);
        }
        ),
    (Deref,
        if( lvalue_is_copy(sp, lv) ) {
        }
        else {
            // HACK: If the dereferenced type is a Box ("owned_box") then hack in move and shallow drop
            if( this->m_lang_Box )
            {
                bool is_box = false;
                with_val_type(sp, *e.val, [&](const auto& ty){
                    DEBUG("ty = " << ty);
                    is_box = this->is_type_owned_box(ty);
                    });
                if( is_box )
                {
                    ::MIR::LValue   inner_lv;
                    // 1. If the inner lvalue isn't a slot with move information, move out of the lvalue into a temporary (with standard temp scope)
                    TU_MATCH_DEF( ::MIR::LValue, (*e.val), (ei),
                    (
                        with_val_type(sp, *e.val, [&](const auto& ty){ inner_lv = this->new_temporary(ty); });
                        this->push_stmt_assign(sp, inner_lv.clone(), ::MIR::RValue( mv$(*e.val) ));
                        *e.val = inner_lv.clone();
                        ),
                    (Variable,
                        inner_lv = ::MIR::LValue(ei);
                        ),
                    (Temporary,
                        inner_lv = ::MIR::LValue(ei);
                        ),
                    (Argument,
                        inner_lv = ::MIR::LValue(ei);
                        )
                    )
                    // 2. Mark the slot as requiring only a shallow drop
                    ::std::vector<VarState> ivs;
                    ivs.push_back(VarState::make_Invalid(InvalidType::Moved));
                    TU_MATCH_DEF( ::MIR::LValue, (inner_lv), (ei),
                    (
                        BUG(sp, "Box move out of invalid LValue " << inner_lv << " - should have been moved");
                        ),
                    (Variable,
                        get_variable_state_mut(sp, ei) = VarState::make_Partial(mv$(ivs));
                        ),
                    (Temporary,
                        get_temp_state_mut(sp, ei.idx) = VarState::make_Partial(mv$(ivs));
                        ),
                    (Argument,
                        TODO(sp, "Mark arg " << ei.idx << " for shallow drop");
                        )
                    )
                    // Early return!
                    return ;
                }
            }
            BUG(sp, "Move out of deref with non-Copy values - &move? - " << lv << " : " << FMT_CB(ss, this->with_val_type(sp, lv, [&](const auto& ty){ss<<ty;});) );
            moved_lvalue(sp, *e.val);
        }
        ),
    (Index,
        if( lvalue_is_copy(sp, lv) ) {
        }
        else {
            BUG(sp, "Move out of index with non-Copy values - Partial move?");
            moved_lvalue(sp, *e.val);
        }
        moved_lvalue(sp, *e.idx);
        ),
    (Downcast,
        // TODO: What if the inner is Copy? What if the inner is a hidden pointer?
        moved_lvalue(sp, *e.val);
        )
    )
}

const ::MIR::LValue& MirBuilder::get_ptr_to_dst(const Span& sp, const ::MIR::LValue& lv) const
{
    // Undo field accesses
    const auto* lvp = &lv;
    while(lvp->is_Field())
        lvp = &*lvp->as_Field().val;

    // TODO: Enum variants?

    ASSERT_BUG(sp, lvp->is_Deref(), "Access of an unsized field without a dereference - " << lv);

    return *lvp->as_Deref().val;
}

// --------------------------------------------------------------------

ScopeHandle::~ScopeHandle()
{
    if( idx != ~0u )
    {
        try {
            ASSERT_BUG(Span(), m_builder.m_scopes.size() > idx, "Scope invalid");
            ASSERT_BUG(Span(), m_builder.m_scopes.at(idx).complete, "Scope " << idx << " not completed");
        }
        catch(...) {
            abort();
        }
    }
}

VarState VarState::clone() const
{
    TU_MATCHA( (*this), (e),
    (Invalid,
        return VarState(e);
        ),
    (Valid,
        return VarState(e);
        ),
    (Optional,
        return VarState(e);
        ),
    (Partial,
        ::std::vector<VarState> n;
        n.reserve(e.size());
        for(const auto& a : e)
            n.push_back( a.clone() );
        return VarState(mv$(n));
        )
    )
    throw "";
}
bool VarState::operator==(VarState& x) const
{
    if( this->tag() != x.tag() )
        return false;
    TU_MATCHA( (*this, x), (te, xe),
    (Invalid,
        return te == xe;
        ),
    (Valid,
        return true;
        ),
    (Optional,
        return te == xe;
        ),
    (Partial,
        if( te.size() != xe.size() )
            return false;
        for(unsigned int i = 0; i < te.size(); i ++)
        {
            if( te[i] != xe[i] )
                return false;
        }
        return true;
        )
    )
    throw "";
}
::std::ostream& operator<<(::std::ostream& os, const VarState& x)
{
    TU_MATCHA( (x), (e),
    (Invalid,
        switch(e)
        {
        case InvalidType::Uninit:   os << "Uninit"; break;
        case InvalidType::Moved:    os << "Moved";  break;
        case InvalidType::Descoped: os << "Descoped";   break;
        }
        ),
    (Valid,
        os << "Valid";
        ),
    (Optional,
        os << "Optional(" << e << ")";
        ),
    (Partial,
        os << "Partial(" << e << ")";
        )
    )
    return os;
}
