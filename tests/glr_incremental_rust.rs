#[path = "../parser.rs"]
mod parser;
#[path = "../token.rs"]
mod token;
#[path = "../forest_checks.rs"]
mod forest_checks;

use parser::*;
use std::sync::{Arc, atomic::{AtomicUsize, Ordering}};

fn calls() -> i32 { ACTION_CALLS.load(Ordering::SeqCst) }

fn main() {
    let mut engine = Parser::new();
    let mut cache = GlrIncrementalCache::new();
    let input = "2 + 3 * 4";
    assert_eq!(engine.run_glr_incremental(input, &mut cache), STATUS_OK);
    let first = calls();
    assert!(first > 0 && !cache.reused_semantic_results && cache.results.is_empty());
    assert_eq!(cache.tokens.len(), 5);
    assert!(cache.forest_node_count > 0 && cache.forest_edge_count > 0);
    assert!(cache.reduce_visit_count > 0);
    assert!(cache.reduce_entries.iter().any(|entry| entry.start_token == 0 && entry.end_token == 5));
    assert_eq!(engine.run_glr_incremental(input, &mut cache), STATUS_OK);
    assert_eq!(calls(), first * 2);

    // The counter is instrumentation for observing skipped semantic actions.
    cache.semantic_reuse = GlrSemanticReusePolicy::Pure;
    assert_eq!(engine.run_glr_incremental(input, &mut cache), STATUS_OK);
    assert_eq!(calls(), first * 3);
    assert!(!cache.reused_semantic_results);
    let results = cache.results.clone();
    assert_eq!(engine.run_glr_incremental(input, &mut cache), STATUS_OK);
    assert!(cache.reused_semantic_results);
    assert_eq!(calls(), first * 3);
    assert_eq!(engine.glr_result_count(), results.len());
    assert_eq!(engine.result(), results[0]);
    for (index, value) in results.iter().enumerate() {
        assert_eq!(engine.glr_result(index), Some(*value));
    }

    SEMANTIC_BIAS.store(1, Ordering::SeqCst);
    cache.semantic_revision += 1;
    assert_eq!(engine.run_glr_incremental(input, &mut cache), STATUS_OK);
    assert!(!cache.reused_semantic_results);
    assert_ne!(cache.results, results);
    assert_eq!(calls(), first * 4);

    let resolver_calls = Arc::new(AtomicUsize::new(0));
    let count = resolver_calls.clone();
    engine.set_ambiguity_resolver(Box::new(move |_, _| {
        count.fetch_add(1, Ordering::SeqCst);
        true
    }));
    for _ in 0..2 {
        let before = calls();
        let resolved = resolver_calls.load(Ordering::SeqCst);
        assert_eq!(engine.run_glr_incremental(input, &mut cache), STATUS_OK);
        assert!(calls() > before && resolver_calls.load(Ordering::SeqCst) > resolved);
        assert!(!cache.reused_semantic_results && cache.results.is_empty());
    }
    engine.clear_ambiguity_resolver();
    assert_eq!(engine.run_glr_incremental(input, &mut cache), STATUS_OK);
    assert!(!cache.reused_semantic_results);
    assert_eq!(engine.run_glr_incremental(input, &mut cache), STATUS_OK);
    assert!(cache.reused_semantic_results);

    for (text, old_begin, old_end, new_begin, new_end) in [
        ("2 + 3 + 4", 3, 4, 3, 4),
        ("2 + 3 + 4 + 5", 5, 5, 5, 7),
        ("2 + 3 + 4", 5, 7, 5, 5),
    ] {
        assert_eq!(engine.run_glr_incremental(text, &mut cache), STATUS_OK);
        let diff = &cache.token_change;
        assert!(diff.changed && !cache.reused_semantic_results);
        assert_eq!((diff.old_begin, diff.old_end, diff.new_begin, diff.new_end),
                   (old_begin, old_end, new_begin, new_end));
        assert!(!cache.invalidated_reduce_entries.is_empty());
        assert!(!cache.forest.is_empty());
        let mut fresh = Parser::new();
        assert_eq!(fresh.run_glr(text), STATUS_OK);
        assert_eq!(fresh.glr_result_count(), engine.glr_result_count());
        for index in 0..fresh.glr_result_count() {
            assert_eq!(fresh.glr_result(index), engine.glr_result(index));
        }
    }
    assert_eq!(engine.run_glr_incremental("2 + 3 + 4", &mut cache), STATUS_OK);
    assert!(cache.reused_semantic_results && !cache.token_change.changed);
    assert!(cache.invalidated_reduce_entries.is_empty());
    assert_eq!(engine.run_glr_incremental(" 2 + 3 + 4", &mut cache), STATUS_OK);
    assert!(!cache.reused_semantic_results && !cache.token_change.changed);
    assert_eq!(cache.tokens[0].offset, 1);

    for _ in 0..2 {
        assert_eq!(engine.run_glr_tree_incremental(input, &mut cache), STATUS_OK);
        assert!(!cache.reused_semantic_results && !cache.semantic_results_reusable);
        assert!(engine.glr_tree(0).is_some());
    }
    cache.semantic_reuse = GlrSemanticReusePolicy::Disabled;
    assert_eq!(engine.run_glr_incremental(input, &mut cache), STATUS_OK);
    assert!(cache.results.is_empty());
    cache.semantic_reuse = GlrSemanticReusePolicy::Pure;
    assert_eq!(engine.run_glr_incremental(input, &mut cache), STATUS_OK);
    assert!(!cache.reused_semantic_results);
    cache.clear();
    assert_eq!(cache.semantic_revision, 1);
    assert_eq!(cache.semantic_reuse, GlrSemanticReusePolicy::Pure);
    assert!(!cache.valid && !cache.semantic_results_reusable);
    for text in ["@", "@", "", ""] {
        assert_ne!(engine.run_glr_incremental(text, &mut cache), STATUS_OK);
        assert!(!cache.reused_semantic_results && !cache.semantic_results_reusable);
        assert_eq!(engine.glr_result_count(), 0);
    }
    assert_eq!(engine.run_glr_incremental(input, &mut cache), STATUS_OK);
    assert!(!cache.reused_semantic_results);
    forest_checks::check_repair();
    println!("Rust GLR incremental and forest repair checks passed");
}
