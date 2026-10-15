use stackvm::{assemble, load_quad_file, Machine};

#[test]
fn bundled_examples_match_expected() {
    let examples = concat!(env!("CARGO_MANIFEST_DIR"), "/../stackvm/examples");
    for entry in std::fs::read_dir(examples).unwrap() {
        let path = entry.unwrap().path();
        if path.extension().is_none_or(|e| e != "quad") {
            continue;
        }
        let file = load_quad_file(&path);
        assert!(file.ok(), "{}: {:?}", path.display(), file.diagnostics);
        let program = assemble(&file);
        assert!(
            program.ok(),
            "{}: {:?}",
            path.display(),
            program.diagnostics
        );
        let input: Vec<_> = file.input.split_whitespace().map(str::to_string).collect();
        let result = Machine::new(false).run(&program, &input, 1_000_000);
        assert!(result.ok, "{}: {}", path.display(), result.error);
        assert_eq!(result.output, file.expected, "{}", path.display());
        assert_eq!(result.text, file.expected_text, "{}", path.display());
        let mut optimized = file.clone();
        stackvm::optimize(&mut optimized, 4);
        let optimized_program = assemble(&optimized);
        assert!(
            optimized_program.ok(),
            "{}: {:?}",
            path.display(),
            optimized_program.diagnostics
        );
        let optimized_result = Machine::new(false).run(&optimized_program, &input, 1_000_000);
        assert!(
            optimized_result.ok,
            "{}: {}",
            path.display(),
            optimized_result.error
        );
        assert_eq!(optimized_result.output, result.output, "{}", path.display());
        assert_eq!(optimized_result.text, result.text, "{}", path.display());
    }
}

fn run(text: &str) -> stackvm::RunResult {
    let file = stackvm::parse_quads(text, "test");
    assert!(file.ok(), "{:?}", file.diagnostics);
    let program = assemble(&file);
    assert!(program.ok(), "{:?}", program.diagnostics);
    Machine::new(false).run(&program, &[], 1000)
}

#[test]
fn heap_errors_and_bounds_checks() {
    let freed=run("(decl, i64, global, p)\n(alloc, 8, _, p)\n(free, p, _, _)\n(load:i32, p, _, x)\n(halt, _, _, _)");
    assert!(!freed.ok && freed.error.contains("堆越界读"));
    let double_free=run("(decl, i64, global, p)\n(alloc, 8, _, p)\n(free, p, _, _)\n(free, p, _, _)\n(halt, _, _, _)");
    assert!(!double_free.ok && double_free.error.contains("free 过了"));
    let zero = run("(alloc, 0, _, p)\n(halt, _, _, _)");
    assert!(!zero.ok && zero.error.contains("必须是正数"));
    let bounds = run("(check, 4, 4, row)\n(halt, _, _, _)");
    assert!(!bounds.ok && bounds.error.contains("数组下标越界") && bounds.error.contains("row"));
    let unknown = run("(call, __vm_unknown, _, x)\n(halt, _, _, _)");
    assert!(!unknown.ok && unknown.error.contains("函数入口不存在"));
}

#[test]
fn unsigned_wide_output_matches_cpp() {
    let result = run(
        "(decl, u64, global, value)\n(:=, -1, _, value)\n(write:u64, value, _, _)\n(halt, _, _, _)",
    );
    assert!(result.ok, "{}", result.error);
    assert_eq!(result.output, "18446744073709551615");
}

#[test]
fn negative_bigint_converts_to_signed_int() {
    let source="(param, -17, _, x)\n(call, __vm_bint_from_int, _, big)\n(param, big, _, value)\n(call, __vm_bint_to_int, _, out)\n(write, out, _, _)\n(param, big, _, handle)\n(call, __vm_object_free, _, _)\n(halt, _, _, _)";
    let result = run(source);
    assert!(result.ok, "{}", result.error);
    assert_eq!(result.output, "-17");
}

#[test]
fn optimization_keeps_control_flow_and_parameters() {
    let mut simple = stackvm::parse_quads(
        "(:=, 2, _, x)\n(+, x, 3, t)\n(write, t, _, _)\n(halt, _, _, _)",
        "fold",
    );
    let stats = stackvm::optimize(&mut simple, 4);
    assert!(stats.constants_folded > 0);
    let output = Machine::new(false).run(&assemble(&simple), &[], 1000);
    assert!(output.ok, "{}", output.error);
    assert_eq!(output.output, "5");

    let source="(param, 41, _, k)\n(call, f, _, _)\n(halt, _, _, _)\n(decl, u32, local, k)\n(label, _, _, f)\n(+:u32, k, 1, r)\n(write:u32, r, _, _)\n(ret, 0, _, _)";
    let mut function = stackvm::parse_quads(source, "function");
    stackvm::optimize(&mut function, 4);
    assert!(function
        .quads
        .iter()
        .any(|q| q.op == "decl" && q.arg(2) == "k"));
    let output = Machine::new(false).run(&assemble(&function), &[], 1000);
    assert!(output.ok, "{}", output.error);
    assert_eq!(output.output, "42");

    let loop_source="(decl, i32, global, i)\n(decl, i32, global, sum)\n(:=, 1, _, i)\n(:=, 0, _, sum)\n(label, _, _, loop)\n(<=:i32, i, 5, t)\n(jz, t, _, done)\n(+, sum, i, sum)\n(+, i, 1, i)\n(j, _, _, loop)\n(label, _, _, done)\n(write, sum, _, _)\n(halt, _, _, _)";
    let plain = stackvm::parse_quads(loop_source, "loop");
    let mut changed = plain.clone();
    stackvm::optimize(&mut changed, 4);
    let a = Machine::new(false).run(&assemble(&plain), &[], 1000);
    let b = Machine::new(false).run(&assemble(&changed), &[], 1000);
    assert!(a.ok && b.ok, "{} / {}", a.error, b.error);
    assert_eq!(a.output, "15");
    assert_eq!(a.output, b.output);
}

#[test]
fn indirect_call_uses_decl_order() {
    let text="(call, entry, _, _)\n(halt, _, _, _)\n(decl, i32, local, x)\n(label, _, _, f)\n(*:i32, x, 2, t)\n(ret, t, _, _)\n(label, _, _, entry)\n(addr, fname, _, p)\n(param, 21, _, ignored)\n(callptr, p, _, r)\n(write, r, _, _)\n(halt, _, _, _)";
    let program = assemble(&stackvm::parse_quads(text, "pointer"));
    assert_eq!(program.parameters["f"], ["x"]);
    assert!(program.exports.contains("f"));
    let mut vm = Machine::new(false);
    vm.set_data(
        stackvm::DataArea::from_parts(
            [vec![0; 8], b"f\0".to_vec()].concat(),
            "fname offset=8 size=2 stride=8 type=string",
        )
        .unwrap(),
    );
    let result = vm.run(&program, &[], 1000);
    assert!(result.ok, "{}", result.error);
    assert_eq!(result.output, "42");
}

#[test]
fn coroutines_yield_resume_and_report_contexts() {
    let text = "(addr, task_a_name, _, task_a_ptr)\n\
(param, task_a_ptr, _, task)\n\
(call, std::Coroutine::create, _, first)\n\
(addr, task_b_name, _, task_b_ptr)\n\
(param, task_b_ptr, _, task)\n\
(call, std::Coroutine::create, _, second)\n\
(param, first, _, coroutine)\n\
(call, std::Coroutine::wait, _, _)\n\
(param, second, _, coroutine)\n\
(call, std::Coroutine::wait, _, _)\n\
(halt, _, _, _)\n\
(label, _, _, task_a)\n\
(putc, 65, _, _)\n\
(call, std::Coroutine::yield, _, _)\n\
(putc, 67, _, _)\n\
(ret, 0, _, _)\n\
(label, _, _, task_b)\n\
(putc, 66, _, _)\n\
(call, std::Coroutine::yelid, _, _)\n\
(putc, 68, _, _)\n\
(ret, 0, _, _)";
    let program = assemble(&stackvm::parse_quads(text, "coroutines"));
    let mut vm = Machine::new(false);
    vm.set_data(
        stackvm::DataArea::from_parts(
            [vec![0; 8], b"task_a\0task_b\0".to_vec()].concat(),
            "task_a_name offset=8 size=7 stride=8 type=string\n\
task_b_name offset=15 size=7 stride=8 type=string",
        )
        .unwrap(),
    );
    let result = vm.run(&program, &[], 1_000);
    assert!(result.ok, "{}", result.error);
    assert_eq!(result.text, "ABCD");
    assert_eq!(result.coroutines.len(), 3);
    assert!(result
        .coroutines
        .iter()
        .all(|context| context.state == "done"));
}

#[test]
fn host_clock_and_instruction_hook_are_precise() {
    let program = assemble(&stackvm::parse_quads(
        "(:=, 2, _, x)\n(+, x, 3, t)\n(write, t, _, _)\n(halt, _, _, _)",
        "hooks",
    ));
    let now = std::rc::Rc::new(std::cell::Cell::new(100_u64));
    let clock = now.clone();
    let calls = std::rc::Rc::new(std::cell::RefCell::new(Vec::new()));
    let observed = calls.clone();
    let mut vm = Machine::new(false);
    vm.set_time_callback(move || {
        let value = clock.get();
        clock.set(value + 75);
        value
    });
    vm.set_instruction_hook(2, move |info| observed.borrow_mut().push(info.clone()));
    let result = vm.run(&program, &[], 100);
    assert!(result.ok, "{}", result.error);
    assert!(result.timing_available);
    assert_eq!(result.started_nanoseconds, 100);
    assert_eq!(result.finished_nanoseconds, 175);
    assert_eq!(result.elapsed_nanoseconds, 75);
    let calls = calls.borrow();
    assert_eq!(
        calls.iter().map(|info| info.steps).collect::<Vec<_>>(),
        [2, 4]
    );
    assert!(calls.iter().all(|info| !info.opcode.is_empty()));
}

#[test]
fn assembly_exposes_stack_instructions() {
    let file = stackvm::parse_quads(
        "(:=, 2, _, x)\n(+, x, 3, t)\n(write, t, _, _)\n(halt, _, _, _)",
        "assembly",
    );
    let program = assemble(&file);
    let lines: Vec<_> = program
        .instructions
        .iter()
        .map(|instruction| instruction.to_text())
        .collect();
    assert_eq!(
        lines,
        ["PUSH 2", "STORE x", "LOAD x", "PUSH 3", "ADD", "STORE t", "LOAD t", "PRINT", "HALT"]
    );
}

#[test]
fn native_strings_bigints_and_containers() {
    let text="(addr, number, _, s)\n(param, s, _, arg)\n(call, __vm_bint_from_str, _, big)\n(param, big, _, a)\n(param, big, _, b)\n(call, __vm_bint_add, _, twice)\n(param, twice, _, v)\n(call, __vm_bint_str, _, decimal)\n(param, decimal, _, s)\n(call, __vm_str_len, _, length)\n(write, length, _, _)\n(param, twice, _, h)\n(call, __vm_object_free, _, _)\n(param, big, _, h)\n(call, __vm_object_free, _, _)\n(call, __vm_vec_new, _, vec)\n(param, vec, _, v)\n(param, 7, _, n)\n(param, 0, _, t)\n(call, __vm_vec_push, _, _)\n(param, vec, _, v)\n(call, __vm_vec_len, _, size)\n(write, size, _, _)\n(param, vec, _, h)\n(call, __vm_object_free, _, _)\n(halt, _, _, _)";
    let source = stackvm::parse_quads(text, "native");
    let program = assemble(&source);
    let mut vm = Machine::new(false);
    let number = b"12345678901234567890\0";
    let data = [vec![0; 8], number.to_vec()].concat();
    vm.set_data(
        stackvm::DataArea::from_parts(
            data,
            &format!("number offset=8 size={} stride=8 type=string", number.len()),
        )
        .unwrap(),
    );
    let result = vm.run(&program, &[], 1000);
    assert!(result.ok, "{}", result.error);
    assert_eq!(result.output, "20 1");
    assert_eq!(result.live_objects, 0);
}

#[test]
fn native_format_reads_string_and_writes_text() {
    let text="(addr, format, _, fmt)\n(param, fmt, _, f)\n(param, 42, _, value)\n(param, 42, _, bits)\n(param, 42, _, hex)\n(call, __vm_print, _, _)\n(halt, _, _, _)";
    let program = assemble(&stackvm::parse_quads(text, "format"));
    let mut vm = Machine::new(false);
    vm.set_data(
        stackvm::DataArea::from_parts(
            [vec![0; 8], b"value=%d binary=%b hex=%x\0".to_vec()].concat(),
            "format offset=8 size=26 stride=8 type=string",
        )
        .unwrap(),
    );
    let result = vm.run(&program, &[], 1000);
    assert!(result.ok, "{}", result.error);
    assert_eq!(result.text, "value=42 binary=101010 hex=2a");
}

#[test]
fn host_registry_dispatches_values_and_context() {
    let text = "(addr, word, _, text)\n(param, text, _, value)\n(call, host_len, _, length)\n(param, length, _, left)\n(param, 2, _, right)\n(call, host_add, _, total)\n(write, total, _, _)\n(halt, _, _, _)";
    let program = assemble(&stackvm::parse_quads(text, "host-registry"));
    let mut registry = stackvm::NativeRegistry::new();
    registry
        .add("host_len", 1, |context, args| {
            Ok(stackvm::Value::Int(
                context.read_string(args[0].int())?.len() as i64,
            ))
        })
        .unwrap();
    registry
        .add("host_add", 2, |_, args| {
            Ok(stackvm::Value::Int(args[0].int() + args[1].int()))
        })
        .unwrap();
    let mut vm = Machine::new(false);
    vm.set_native_registry(registry);
    vm.set_data(
        stackvm::DataArea::from_parts(
            [vec![0; 8], b"rust\0".to_vec()].concat(),
            "word offset=8 size=5 stride=8 type=string",
        )
        .unwrap(),
    );
    let result = vm.run(&program, &[], 1000);
    assert!(result.ok, "{}", result.error);
    assert_eq!(result.output, "6");
}

#[test]
fn host_registry_rejects_reserved_vm_symbols() {
    let mut registry = stackvm::NativeRegistry::new();
    let error = registry
        .add("__vm_print", 0, |_, _| Ok(stackvm::Value::Int(0)))
        .unwrap_err();
    assert!(error.contains("保留"), "{error}");
}

#[test]
fn ordered_set_and_map_copy_text_values() {
    let source="(addr, key, _, k)\n(call, __vm_set_new, _, s)\n(param, s, _, h)\n(param, k, _, key)\n(param, 1, _, text)\n(call, __vm_set_add, _, _)\n(param, s, _, h)\n(param, k, _, key)\n(param, 1, _, text)\n(call, __vm_set_has, _, has)\n(write, has, _, _)\n(call, __vm_map_new, _, m)\n(param, m, _, h)\n(param, k, _, key)\n(param, 1, _, key_text)\n(param, 9, _, value)\n(param, 0, _, value_text)\n(call, __vm_map_put, _, _)\n(param, m, _, h)\n(param, k, _, key)\n(param, 1, _, key_text)\n(param, 0, _, value_text)\n(call, __vm_map_get, _, v)\n(write, v, _, _)\n(param, m, _, h)\n(param, 0, _, index)\n(param, 1, _, text)\n(call, __vm_map_key_at, _, copy)\n(param, copy, _, address)\n(call, __vm_str_len, _, length)\n(write, length, _, _)\n(free, copy, _, _)\n(param, s, _, h)\n(call, __vm_object_free, _, _)\n(param, m, _, h)\n(call, __vm_object_free, _, _)\n(halt, _, _, _)";
    let program = assemble(&stackvm::parse_quads(source, "containers"));
    let mut vm = Machine::new(false);
    vm.set_data(
        stackvm::DataArea::from_parts(
            [vec![0; 8], b"key\0".to_vec()].concat(),
            "key offset=8 size=4 stride=8 type=string",
        )
        .unwrap(),
    );
    let result = vm.run(&program, &[], 1000);
    assert!(result.ok, "{}", result.error);
    assert_eq!(result.output, "1 9 3");
    assert_eq!(result.live_objects, 0);
}
