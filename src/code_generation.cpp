#include "code_generation.h"

unique_ptr<Module> code_gen_nodes(const Nodes &nodes)
{
    modules[current_module_pointer] = std::make_unique<Module>("TheModule", context);

    initialize_function_pass_manager();

    for (auto &node : nodes)
    {
        code_gen_node(std::move(node));
    }

    print_current_module();

    return std::move(modules[current_module_pointer]);
}

Value *code_gen_node(const unique_ptr<Node> &node)
{
    node->code_gen();
    return 0;
}

void initialize_function_pass_manager()
{
    fpm = std::make_unique<llvm::legacy::FunctionPassManager>(modules[current_module_pointer].get());
    fpm->add(llvm::createInstructionCombiningPass());
    fpm->add(llvm::createReassociatePass());
    fpm->add(llvm::createGVNPass());
    fpm->add(llvm::createCFGSimplificationPass());

    fpm->doInitialization();
}

void print_v(Value *v)
{
    v->print(outs());
    outs() << '\n';
}

void print_t(Type *t)
{
    t->print(outs());
    outs() << '\n';
}

Value *Function_Node::code_gen()
{
    auto function = prototype->code_gen_proto();
    if (function == 0)
        return 0;

    auto entry = BasicBlock::Create(context, "entry", function);
    builder.SetInsertPoint(entry);

    current_function_name = prototype->get_name();
    functions[current_function_name] = this;

    scope = Scope::function;

    prototype->create_argument_allocas(function);

    if (prototype->get_name() == "main" && global_variables_awaiting_initialization.size() > 0)
    {
        std::vector<llvm::Value *> call_args(0);
        auto callee_f = modules[current_module_pointer]->getFunction(global_variable_assign_function_name);
        builder.CreateCall(callee_f, call_args);
    }

    currently_preferred_type = prototype->get_return_type();

    then->code_gen();

    if (!entry->getTerminator())
    {
        builder.CreateRetVoid();
    }

    if (optimize)
        fpm->run(*function);

    if (verifyFunction(*function, &outs()))
    {
        print_current_module();
        exit(1);
    }

    return function;
}

void Prototype_Node::create_argument_allocas(Function *function)
{
    Function::arg_iterator it = function->arg_begin();
    for (unsigned i = 0, size = param_names.size(); i != size; ++i, ++it)
    {
        auto ptr = create_entry_block_alloca(function, param_names[i], variable_type_to_llvm_type(param_types[i]));
        auto store = builder.CreateStore(it, ptr);
        auto loaded = builder.CreateLoad(ptr);
        functions[current_function_name]->set_variable_ptr(param_names[i], ptr);
        functions[current_function_name]->set_variable_value(param_names[i], loaded);
    }
}

Value *create_entry_block_alloca(Function *function, const std::string &name, Type *type)
{
    IRBuilder<> tmp_builder(&function->getEntryBlock(), function->getEntryBlock().begin());
    return tmp_builder.CreateAlloca(type, 0, name.c_str());
}

Function *Prototype_Node::code_gen_proto()
{
    std::vector<Type *> types;
    std::map<int, llvm::Type *> dereferenced_types;
    int i = 0;
    for (auto &param_type : param_types)
    {
        llvm::Type *llvm_type;
        if (param_type == Variable_Type::type_object || param_type == Variable_Type::type_object_ptr || param_type == Variable_Type::type_object_ref)
        {
            auto parameter_type_names = get_parameter_type_names();
            llvm_type = variable_type_to_llvm_type(param_type, parameter_type_names[i]);
        }
        else
        {
            llvm_type = variable_type_to_llvm_type(param_type);
        }

        if (is_reference_type(param_type))
            dereferenced_types[i] = llvm_type;
        types.push_back(llvm_type);
        i++;
    }

    Type *ret;
    if (return_type == Variable_Type::type_object)
        ret = variable_type_to_llvm_type(return_type, return_type_name);
    else
        ret = variable_type_to_llvm_type(return_type);

    llvm::FunctionType *function_type = llvm::FunctionType::get(ret, types, false);
    llvm::Function *f = llvm::Function::Create(function_type, llvm::Function::ExternalLinkage, name, modules[current_module_pointer].get());

    auto dl = new llvm::DataLayout(modules[current_module_pointer].get());
    auto it = dereferenced_types.begin();
    while (it != dereferenced_types.end())
    {
        unsigned bytes = dl->getTypeAllocSize(it->second);
        f->addDereferenceableAttr(it->first + 1, bytes);
        it++;
    }

    if (f->getName() != name)
    {
        f->eraseFromParent();
        f = modules[current_module_pointer]->getFunction(name);

        if (!f->empty())
            error("Redefinition of function");

        if (f->arg_size() != param_names.size())
            error("Redefinition of function with different number of arguments");
    }

    unsigned idx = 0;
    for (llvm::Function::arg_iterator ai = f->arg_begin(); idx != param_names.size(); ai++, idx++)
    {
        ai->setName(param_names[idx]);
    }

    return f;
}

Value *Expression_Node::code_gen()
{
    return 0;
}
Value *Binary_Operation_Expression_Node::code_gen()
{
    auto l = lhs->code_gen();
    auto r = rhs->code_gen();

    if (l == 0 || r == 0)
    {
        error("Error in binary operation codegen");
        return 0;
    }

    auto l_loaded = load_if_ptr(l);
    auto r_loaded = load_if_ptr(r);

    auto l_type = l_loaded->getType();
    auto r_type = r_loaded->getType();

    if (l_type->isDoubleTy() || l_type->isFloatTy() || r_type->isDoubleTy() || r_type->isFloatTy())
    {
        if (op == "+")
            return builder.CreateFAdd(l_loaded, r_loaded, "addtmp");
        if (op == "-")
            return builder.CreateFSub(l_loaded, r_loaded, "subtmp");
        if (op == "*")
            return builder.CreateFMul(l_loaded, r_loaded, "multmp");
        if (op == "<")
            return builder.CreateFCmpOLT(l_loaded, r_loaded, "lttmp");
    }
    else
    {
        if (op == "+")
            return builder.CreateAdd(l_loaded, r_loaded, "addtmp");
        if (op == "-")
            return builder.CreateSub(l_loaded, r_loaded, "subtmp");
        if (op == "*")
            return builder.CreateMul(l_loaded, r_loaded, "multmp");
        if (op == "<")
            return builder.CreateICmpSLT(l_loaded, r_loaded, "lttmp");
    }

    return 0;
}

Value *Number_Expression_Node::code_gen()
{
    if (variable_type == Variable_Type::type_null)
        variable_type = currently_preferred_type;
    switch (variable_type)
    {
    case Variable_Type::type_i64:
        return llvm::ConstantInt::get(context, llvm::APInt(64, (int)value, true));
    case Variable_Type::type_i32:
        return llvm::ConstantInt::get(context, llvm::APInt(32, (int)value, true));
    case Variable_Type::type_i16:
        return llvm::ConstantInt::get(context, llvm::APInt(16, (int)value, true));
    case Variable_Type::type_i8:
        return llvm::ConstantInt::get(context, llvm::APInt(8, (int)value, true));
    case Variable_Type::type_float:
        return llvm::ConstantFP::get(context, llvm::APFloat((float)value));
    case Variable_Type::type_double:
        return llvm::ConstantFP::get(context, llvm::APFloat((double)value));
    case Variable_Type::type_bool:
        return llvm::ConstantInt::get(context, llvm::APInt(1, (int)value, true));
    default:
        error("Could not codegen number");
        return nullptr;
    }
}

Value *Prototype_Node::code_gen()
{
    return 0;
}
Value *Then_Node::code_gen()
{
    for (auto &node : nodes)
    {
        node->code_gen();
    }
    return 0;
}
Value *Variable_Declaration_Node::code_gen()
{
    if (type == Variable_Type::type_object)
        return code_gen_object_variable_declaration(this);
    else if (type == Variable_Type::type_array)
        return code_gen_array_variable_declaration(this);
    else
        return code_gen_primitive_variable_declaration(this);
}

Value *code_gen_array_variable_declaration(Variable_Declaration_Node *var)
{
    currently_preferred_type = var->get_array_type();
    auto members_type = variable_type_to_llvm_type(var->get_array_type());
    auto members = var->get_value()->get_members();
    auto array_type = llvm::ArrayType::get(members_type, members.size());
    auto ptr = builder.CreateAlloca(array_type, 0, var->get_name());

    int i = 0;
    for (auto &member : members)
    {
        auto gep = builder.CreateStructGEP(ptr, i);
        auto store = builder.CreateStore(member->code_gen(), gep);
        i++;
    }

    currently_preferred_type = Variable_Type::type_i32;

    return 0;
}

Value *code_gen_object_variable_declaration(Variable_Declaration_Node *var)
{
    auto llvm_ty = object_types[var->get_type_name()];
    print_t(llvm_ty);
    auto ptr = builder.CreateAlloca(llvm_ty, 0, var->get_name());

    define_object_properties(var, ptr, var->get_value());

    auto loaded = builder.CreateLoad(ptr);

    functions[current_function_name]->set_variable_ptr(var->get_name(), ptr);
    functions[current_function_name]->set_variable_value(var->get_name(), loaded);

    return 0;
}

void define_object_properties(Variable_Declaration_Node *var, Value *ptr, unique_ptr<Node> expr)
{
    auto properties = expr->get_properties();
    auto it = properties.begin();
    for (unsigned i = 0; it != properties.end(); i++)
    {
        auto variable_type = object_type_properties[var->get_type_name()][it->first];
        currently_preferred_type = variable_type;

        auto index = APInt(32, i);
        auto index_value = Constant::getIntegerValue(Type::getInt32Ty(context), index);

        auto v = load_if_ptr(it->second->code_gen());
        auto val_ptr = builder.CreateStructGEP(ptr, i, it->first + "_ptr");
        auto store = builder.CreateStore(v, val_ptr);

        it++;
    }
}

Value *code_gen_primitive_variable_declaration(Variable_Declaration_Node *var)
{
    auto llvm_ty = variable_type_to_llvm_type(var->get_type());
    auto ptr = builder.CreateAlloca(llvm_ty, 0, var->get_name());

    if (var->is_undefined())
    {
        functions[current_function_name]->set_variable_ptr(var->get_name(), ptr);
        return ptr;
    }

    auto v = var->get_value()->code_gen();
    auto store = builder.CreateStore(v, ptr);

    if (var->get_type() == Variable_Type::type_i32_ptr)
    {
        functions[current_function_name]->set_variable_ptr(var->get_name(), ptr);
        return v;
    }

    auto loaded = builder.CreateLoad(ptr);

    functions[current_function_name]->set_variable_ptr(var->get_name(), ptr);
    functions[current_function_name]->set_variable_value(var->get_name(), loaded);

    return loaded;
}

Value *If_Node::code_gen()
{
    std::vector<Value *> cmps;
    for (auto &condition : conditions)
    {
        auto cmp = condition->code_gen();
        cmps.push_back(cmp);
    }

    Value *last_cmp = cmps[0];
    int i = 0;
    for (auto &sep : condition_separators)
    {
        switch (sep)
        {
        case Token_Type::tok_and:
            if (i == 0)
                last_cmp = builder.CreateAnd(cmps[i], cmps[i + 1]);
            else
                last_cmp = builder.CreateAnd(last_cmp, cmps[i + 1]);
            break;
        case Token_Type::tok_or:
        {
            if (i == 0)
                last_cmp = builder.CreateOr(cmps[i], cmps[i + 1]);
            else
                last_cmp = builder.CreateOr(last_cmp, cmps[i + 1]);
        }
        default:
            break;
        }
        i++;
    }

    auto function = builder.GetInsertBlock()->getParent();

    auto then_bb = BasicBlock::Create(context, "then", function);
    auto else_bb = BasicBlock::Create(context, "else");
    auto continue_bb = BasicBlock::Create(context, "continue");

    auto cond_br = builder.CreateCondBr(last_cmp, then_bb, else_bb);

    builder.SetInsertPoint(then_bb);

    then->code_gen();

    builder.CreateBr(continue_bb);

    then_bb = builder.GetInsertBlock();

    function->getBasicBlockList().push_back(else_bb);
    builder.SetInsertPoint(else_bb);

    builder.CreateBr(continue_bb);

    else_bb = builder.GetInsertBlock();

    function->getBasicBlockList().push_back(continue_bb);
    builder.SetInsertPoint(continue_bb);

    return 0;
}
Value *For_Node::code_gen()
{
    auto entry_bb = builder.GetInsertBlock();
    auto function = entry_bb->getParent();

    auto loop_bb = BasicBlock::Create(context, "loop", function);

    builder.CreateBr(loop_bb);

    builder.SetInsertPoint(loop_bb);

    auto start_val = variable->get_value()->code_gen();
    auto var = variable->code_gen();
    auto phi = builder.CreatePHI(start_val->getType(), 2);
    phi->addIncoming(start_val, entry_bb);
    phi->addIncoming(var, loop_bb);

    print_v(phi);

    // auto v =
    print_v(start_val);

    // auto pre_bb = builder.GetInsertBlock();
    // auto function = builder.GetInsertBlock()->getParent();
    // auto var = variable->code_gen();
    // auto cond = condition->code_gen();
    // auto loop_bb = llvm::BasicBlock::Create(context, "loop", function);
    // auto continue_bb = llvm::BasicBlock::Create(context, "continue", function);
    // auto br = builder.CreateCondBr(cond, loop_bb, continue_bb);
    // builder.SetInsertPoint(loop_bb);

    // auto variable = builder.CreatePHI(Type::getInt32Ty(context),
    //                                   2, var->getName().str());
    // print_v(var);
    // variable->addIncoming(var, pre_bb);
    // variable->addIncoming(, loop_bb);

    // assignment->code_gen();

    // then->code_gen();

    // auto new_cond = condition->code_gen();
    // auto repeat = builder.CreateCondBr(new_cond, loop_bb, continue_bb);

    // builder.SetInsertPoint(continue_bb);

    return 0;
}
Value *Condition_Node::code_gen()
{
    auto l = load_if_ptr(lhs->code_gen());
    auto r = load_if_ptr(rhs->code_gen());

    switch (op)
    {
    case Token_Type::tok_compare_eq:
        return builder.CreateICmpEQ(l, r, "ifcond");
    case Token_Type::tok_compare_lt:
        return builder.CreateICmpSLT(l, r, "ltcond");
    case Token_Type::tok_compare_gt:
        return builder.CreateICmpSGT(l, r, "gtcond");
    default:
        error("Unknown operator in condition");
        return 0;
    }
}
Value *Function_Call_Node::code_gen()
{
    auto callee_function = modules[current_module_pointer]->getFunction(name);
    if (callee_function == 0)
        error("Unknown function referenced in function call");
    if (callee_function->arg_size() != parameters.size())
        error("Incorrect number of parameters passed to function call");

    std::vector<llvm::Type *> arg_types;
    auto args = callee_function->args();
    auto it = args.begin();
    while (it != args.end())
    {
        arg_types.push_back(it->getType());
        it++;
    }

    std::vector<llvm::Value *> args_v;
    std::map<int, uint64_t> dereferenced_types;
    for (unsigned i = 0, size = parameters.size(); i != size; i++)
    {
        auto bytes = callee_function->getParamDereferenceableBytes(i);
        llvm::Value *v;

        if (bytes)
        {
            wants_reference = true;
            v = parameters[i]->code_gen();
            dereferenced_types[i] = bytes;
            args_v.push_back(v);
            wants_reference = false;
        }
        else
        {
            v = parameters[i]->code_gen();
            args_v.push_back(v);
        }
    }

    auto call = builder.CreateCall(callee_function, args_v, "calltmp");

    auto dereferenced_it = dereferenced_types.begin();
    while (dereferenced_it != dereferenced_types.end())
    {
        call->addDereferenceableAttr(dereferenced_it->first + 1, dereferenced_it->second);
        dereferenced_it++;
    }

    return call;
}
Value *Variable_Expression_Node::code_gen()
{
    if (wants_reference)
        return functions[current_function_name]->get_variable_ptr(name);
    if (type == Variable_Expression_Type::reference)
        return functions[current_function_name]->get_variable_ptr(name);
    else if (type == Variable_Expression_Type::pointer)
    {
        auto ptr = builder.CreateLoad(functions[current_function_name]->get_variable_ptr(name));
        auto val = builder.CreateLoad(ptr);
        return val;
    }
    else
    {
        auto ptr = functions[current_function_name]->get_variable_ptr(name);
        return builder.CreateLoad(ptr);
    }
}
Value *Import_Node::code_gen()
{
    return 0;
}
Value *Variable_Assignment_Node::code_gen()
{
    auto v = value->code_gen();
    auto reference_variable_names = functions[current_function_name]->get_reference_variable_names();
    for (auto &ref_name : reference_variable_names)
    {
        if (name == ref_name)
        {
            return builder.CreateStore(v, functions[current_function_name]->get_variable_value(name));
        }
    }
    return builder.CreateStore(v, functions[current_function_name]->get_variable_ptr(name));
}
Value *Object_Node::code_gen()
{
    auto it = properties.begin();

    std::vector<llvm::Type *> members;
    while (it != properties.end())
    {
        auto ty = variable_type_to_llvm_type(it->second);
        members.push_back(ty);

        object_type_properties[name][it->first] = it->second;

        it++;
    }

    llvm::ArrayRef<llvm::Type *> struct_properties(members);
    auto struct_type = llvm::StructType::create(context, struct_properties, name, false);

    object_types[name] = struct_type;

    cout << "object node: " << endl;

    print_t(object_types[name]);

    return 0;
}
Value *Object_Expression::code_gen()
{

    return 0;
}
Value *String_Expression::code_gen()
{
    return 0;
}

Value *Return_Node::code_gen()
{
    auto v = value->code_gen();
    auto ret = builder.CreateRet(v);
    return ret;
}

Value *Array_Expression::code_gen()
{
    return 0;
}

Type *variable_type_to_llvm_type(Variable_Type type, std::string object_type_name)
{
    // auto it = object_types.begin();
    // // cout << object_types.size() << endl;
    // while (it != object_types.end())
    // {
    //     // cout << "type: " << it->first << endl;
    //     print_t(it->second);
    //     it++;
    // }

    switch (type)
    {
    case Variable_Type::type_i64:
        return Type::getInt64Ty(context);
    case Variable_Type::type_i32:
        return Type::getInt32Ty(context);
    case Variable_Type::type_i16:
        return Type::getInt16Ty(context);
    case Variable_Type::type_i8:
        return Type::getInt8Ty(context);
    case Variable_Type::type_bool:
        return Type::getInt1Ty(context);
    case Variable_Type::type_float:
        return Type::getFloatTy(context);
    case Variable_Type::type_double:
        return Type::getDoubleTy(context);
    case Variable_Type::type_object:
        return object_types[object_type_name];
    case Variable_Type::type_i64_ptr:
        return Type::getInt64PtrTy(context);
    case Variable_Type::type_i32_ptr:
        return Type::getInt32PtrTy(context);
    case Variable_Type::type_i16_ptr:
        return Type::getInt16PtrTy(context);
    case Variable_Type::type_i8_ptr:
        return Type::getInt8PtrTy(context);
    case Variable_Type::type_bool_ptr:
        return Type::getInt1PtrTy(context);
    case Variable_Type::type_float_ptr:
        return Type::getFloatPtrTy(context);
    case Variable_Type::type_double_ptr:
        return Type::getDoublePtrTy(context);
    case Variable_Type::type_object_ptr:
    {
        // ! WHY THE FUCK CAN'T I JUST DO OBJECT_TYPES[OBJECT_TYPE_NAME] ????? WHAT?
        auto it = object_types.begin();
        while (it != object_types.end())
        {
            if (it->first == object_type_name)
                return llvm::PointerType::get(it->second, 0);
            it++;
        }
    }
    case Variable_Type::type_i64_ref:
        return Type::getInt64PtrTy(context);
    case Variable_Type::type_i32_ref:
        return Type::getInt32PtrTy(context);
    case Variable_Type::type_i16_ref:
        return Type::getInt16PtrTy(context);
    case Variable_Type::type_i8_ref:
        return Type::getInt8PtrTy(context);
    case Variable_Type::type_bool_ref:
        return Type::getInt1PtrTy(context);
    case Variable_Type::type_float_ref:
        return Type::getFloatPtrTy(context);
    case Variable_Type::type_double_ref:
        return Type::getDoublePtrTy(context);
    default:
        error("Could not convert variable type to llvm type");
        return nullptr;
    }
}

bool is_reference_type(Variable_Type type)
{
    switch (type)
    {
    case Variable_Type::type_i64:
        return false;
    case Variable_Type::type_i32:
        return false;
    case Variable_Type::type_i16:
        return false;
    case Variable_Type::type_i8:
        return false;
    case Variable_Type::type_bool:
        return false;
    case Variable_Type::type_float:
        return false;
    case Variable_Type::type_double:
        return false;
    case Variable_Type::type_object:
        return false;
    case Variable_Type::type_i64_ptr:
        return false;
    case Variable_Type::type_i32_ptr:
        return false;
    case Variable_Type::type_i16_ptr:
        return false;
    case Variable_Type::type_i8_ptr:
        return false;
    case Variable_Type::type_bool_ptr:
        return false;
    case Variable_Type::type_float_ptr:
        return false;
    case Variable_Type::type_double_ptr:
        return false;
    case Variable_Type::type_object_ptr:
        return false;
    case Variable_Type::type_i64_ref:
        return true;
    case Variable_Type::type_i32_ref:
        return true;
    case Variable_Type::type_i16_ref:
        return true;
    case Variable_Type::type_i8_ref:
        return true;
    case Variable_Type::type_bool_ref:
        return true;
    case Variable_Type::type_float_ref:
        return true;
    case Variable_Type::type_double_ref:
        return true;
    default:
        error("Could not determine if variable type is reference type");
        return false;
    }
}

Value *load_if_ptr(Value *v)
{
    if (v->getType()->isPointerTy())
        return builder.CreateLoad(v);
    return v;
}

void print_current_module()
{
    std::error_code ec;
    auto f_out = raw_fd_ostream("../out.ll", ec);
    llvm::raw_ostream &os = llvm::outs();
    os << '\n'
       << '\n';
    auto writer = new AssemblyAnnotationWriter();
    modules[current_module_pointer]->print(os, writer);
    modules[current_module_pointer]->print(f_out, writer);
}

void error(const char *arg)
{
    cout << arg << endl;

    print_current_module();

    exit(1);
}