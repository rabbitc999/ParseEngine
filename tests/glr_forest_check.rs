use crate::parser::*;
use std::collections::{HashMap, HashSet};

fn signature(cache: &GlrIncrementalCache) -> Vec<String> {
    let nodes: HashMap<_, _> = cache.forest.iter().map(|node| (node.id, node)).collect();
    assert_eq!(nodes.len(), cache.forest.len());
    let key = |id: &u64| {
        let node = nodes[id];
        format!("{}:{}:{}", node.symbol, node.start_token, node.end_token)
    };
    let mut rows = Vec::new();
    let mut live = HashSet::new();
    let mut work = cache.forest_roots.clone();
    while let Some(id) = work.pop() {
        if !live.insert(id) { continue; }
        let node = nodes[&id];
        assert!(node.start_token <= node.end_token && node.end_token <= cache.tokens.len());
        assert!(node.terminal || !node.alternatives.is_empty());
        rows.push(format!("node:{}:{}", key(&id), node.text));
        for alternative in &node.alternatives {
            let mut row = format!("{}={}", key(&id), alternative.production);
            for child in &alternative.children {
                assert!(nodes[child].parents.contains(&id));
                row.push_str(&format!("/{}", key(child)));
                work.push(*child);
            }
            rows.push(row);
        }
        for parent in &node.parents {
            assert!(nodes[parent].alternatives.iter().any(|alt| alt.children.contains(&id)));
        }
    }
    assert_eq!(live.len(), nodes.len());
    for root in &cache.forest_roots { rows.push(format!("root:{}", key(root))); }
    rows.sort();
    rows
}

pub fn check_edits(inputs: &[&str]) {
    let mut engine = Parser::new();
    let mut cache = GlrIncrementalCache::new();
    for input in inputs {
        let old = cache.clone();
        let status = engine.run_glr_tree_incremental(input, &mut cache);
        let mut fresh = Parser::new();
        let mut baseline = GlrIncrementalCache::new();
        assert_eq!(fresh.run_glr_tree_incremental(input, &mut baseline), status);
        assert_eq!(signature(&cache), signature(&baseline));
        assert_eq!(engine.errors().len(), fresh.errors().len());
        for (left, right) in engine.errors().iter().zip(fresh.errors()) {
            assert_eq!(left.to_string(), right.to_string());
        }
        assert_eq!(engine.glr_result_count(), fresh.glr_result_count());
        for i in 0..engine.glr_result_count() {
            assert_eq!(engine.glr_tree(i).unwrap().to_string(), fresh.glr_tree(i).unwrap().to_string());
        }
        let invalid: HashSet<_> = cache.invalidated_forest_nodes.iter().copied().collect();
        for node in &old.forest {
            if invalid.contains(&node.id) {
                for parent in &node.parents { assert!(invalid.contains(parent)); }
            }
        }
        for node in &cache.forest { assert!(!invalid.contains(&node.id)); }
        assert_eq!(cache.forest.len(), cache.reused_forest_node_count + cache.rebuilt_forest_node_count);
    }
}

pub fn check_repair() {
    let mut engine = Parser::new();
    let mut cache = GlrIncrementalCache::new();
    assert_eq!(engine.run_glr_tree_incremental("2 + 3 * 4", &mut cache), STATUS_OK);
    assert!(cache.forest_roots.len() >= 2);
    let root = cache.forest_roots[0];
    assert_eq!(cache.forest.iter().find(|node| node.id == root).unwrap().alternatives.len(), 2);
    let token = |cache: &GlrIncrementalCache| cache.forest.iter()
        .find(|node| node.terminal && node.text == "4").unwrap().id;
    let suffix = token(&cache);
    assert_eq!(engine.run_glr_tree_incremental("2 + 8 * 4", &mut cache), STATUS_OK);
    assert!(cache.invalidated_forest_nodes.contains(&root));
    assert!(cache.reused_forest_node_count > 0 && cache.rebuilt_forest_node_count > 0);
    assert_eq!(token(&cache), suffix);
    assert_eq!(engine.run_glr_tree_incremental("0 + 2 + 8 * 4", &mut cache), STATUS_OK);
    assert_eq!(token(&cache), suffix);
    assert_eq!(cache.forest.iter().find(|node| node.id == suffix).unwrap().start_token, 6);
    check_edits(&["2 + 3 * 4", "2 + 8 * 4", "0 + 2 + 8 * 4", "2 + 8 * 4",
                  " 2 +\n8 * 4", "2 * 8 + 4", "2 + + 4", "@", "", "2 + 3"]);
}
