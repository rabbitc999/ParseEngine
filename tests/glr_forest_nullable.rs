#[path = "../parser.rs"]
mod parser;
#[path = "../token.rs"]
mod token;
#[path = "../forest_checks.rs"]
mod forest_checks;

fn main() {
    forest_checks::check_edits(&["", "a b c", "a a b c", "a a b c b b", "a b c b b",
                                "a b c", "", "", "a b b c", "a b c", "@", ""]);
    println!("Rust nullable forest repair checks passed");
}
