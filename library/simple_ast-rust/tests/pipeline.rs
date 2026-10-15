use simple_ast::{generate, Function, Node, Program, Type};
use stackvm::{assemble, parse_quads, DataArea, Machine};

#[test]
fn generated_quads_and_data_run_in_rust_vm() {
    let mut source = Program::default();
    source.add_variable("arr", Type::array(Type::i32(), "count=3;align=8"));
    source.add_variable("answer", Type::i32());
    let body = Node::block(vec![
        Node::assign(
            "arr",
            Node::ArrayNew(
                Type::array(Type::i32(), "count=3;align=8"),
                vec![Node::i(10), Node::i(20), Node::i(30)],
            ),
        ),
        Node::assign(
            "answer",
            Node::Index(Box::new(Node::var("arr")), Box::new(Node::i(2))),
        ),
        Node::write(Node::var("answer")),
        Node::ret(Node::i(0)),
    ]);
    source.functions.push(Function::new("main", body));
    let generated = generate(&source, "pipeline");
    assert!(generated.ok(), "{:?}", generated.diagnostics);
    let file = parse_quads(&generated.quads, "generated");
    assert!(file.ok(), "{:?}", file.diagnostics);
    let program = assemble(&file);
    assert!(program.ok(), "{:?}", program.diagnostics);
    let layout = generated.export_data_layout().join("\n");
    let data = DataArea::from_parts(generated.export_data_binary(), &layout).unwrap();
    let mut vm = Machine::new(false);
    vm.set_data(data);
    let result = vm.run(&program, &[], 10000);
    assert!(result.ok, "{}", result.error);
    assert_eq!(result.output, "30");
}

#[test]
fn type_and_layout_match_cpp_contract() {
    let mut source = Program::default();
    source
        .types
        .push(Type::struct_of("Vec", vec![("x".into(), Type::f64())]));
    source.add_operator("Vec", "+", "add");
    source.add_method(
        "Vec",
        Function::new("add", Node::block(vec![Node::ret(Node::var("self"))])),
    );
    assert!(source.types[0].describe().contains("methods: add"));
    assert!(source.types[0].describe().contains("operators: +→add"));
    source.add_variable("p", Type::reference(Type::i32()));
    source.functions.push(Function::new(
        "main",
        Node::block(vec![
            Node::assign("p", Node::Null),
            Node::ExprStmt(Box::new(Node::str("hello"))),
            Node::ret(Node::i(0)),
        ]),
    ));
    let result = generate(&source, "layout");
    assert!(result.ok(), "{:?}", result.diagnostics);
    assert!(result.quads.contains("(decl, i64, local, self)"));
    assert!(result.quads.contains("(:=, 0, _, p)"));
    assert!(result.export_data_layout()[0].contains("offset=8"));
    assert!(result
        .export_data_binary()
        .windows(6)
        .any(|w| w == b"hello\0"));
    assert!(result
        .export_data_header("layout")
        .contains("DATA_str_1_OFFSET 8u"));
}

#[test]
fn struct_method_and_float_data_execute() {
    let mut source = Program::default();
    source
        .types
        .push(Type::struct_of("Point", vec![("x".into(), Type::f64())]));
    source.add_variable(
        "point",
        Type::struct_of("Point", vec![("x".into(), Type::f64())]),
    );
    let mut get = Function::new(
        "get",
        Node::block(vec![Node::ret(Node::Member(
            Box::new(Node::var("self")),
            "x".into(),
        ))]),
    );
    get.result = Type::f64();
    source.add_method("Point", get);
    source.functions.push(Function::new(
        "main",
        Node::block(vec![
            Node::assign("point", Node::StructNew("Point".into(), vec![Node::f(1.5)])),
            Node::write(Node::MethodCall(
                Box::new(Node::var("point")),
                "get".into(),
                vec![],
            )),
            Node::ret(Node::i(0)),
        ]),
    ));
    let generated = generate(&source, "point");
    assert!(generated.ok(), "{:?}", generated.diagnostics);
    let data = DataArea::from_parts(
        generated.export_data_binary(),
        &generated.export_data_layout().join("\n"),
    )
    .unwrap();
    let program = assemble(&parse_quads(&generated.quads, "point"));
    assert!(program.ok(), "{:?}", program.diagnostics);
    let mut vm = Machine::new(false);
    vm.set_data(data);
    let result = vm.run(&program, &[], 1000);
    assert!(result.ok, "{}", result.error);
    assert_eq!(result.output, "1.5");
}

#[test]
fn enum_lambda_and_reference_checks() {
    let mut source = Program::default();
    source.types.push(Type::enum_of(
        "Maybe",
        vec![("None".into(), None), ("Some".into(), Some(Type::i32()))],
    ));
    source.add_variable("p", Type::reference(Type::i32()));
    source.functions.push(Function::new(
        "main",
        Node::block(vec![
            Node::expr_stmt(Node::enum_new("Maybe", "Some", vec![Node::i(7)])),
            Node::expr_stmt(Node::lambda(Node::block(vec![]))),
            Node::expr_stmt(Node::binary("+", Node::var("p"), Node::i(1))),
            Node::ret(Node::i(0)),
        ]),
    ));
    let result = generate(&source, "features");
    assert!(!result.ok());
    assert!(result.diagnostics[0].contains("不做指针运算"));
    assert!(result.data.contains("Some(7)"));
    assert!(result.data.contains("lambda_"));
    let enum_layout = result.export_data_layout();
    assert!(enum_layout
        .iter()
        .any(|line| line.contains("enum_") && line.contains("size=12")));
    assert!(result
        .export_data_binary()
        .windows(4)
        .any(|w| w == 1u32.to_le_bytes()));
}
