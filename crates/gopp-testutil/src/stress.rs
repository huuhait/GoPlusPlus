//! Generated Go++ sources that stress nesting depth and sibling count.

/// Largest source a generator returns; asking for more is a bug in the test.
pub const MAX_BYTES: usize = 16 << 20;

/// One expression nested `depth` parentheses deep.
pub fn deep_expr(depth: usize) -> String {
    function(&format!(
        "\tx = {}x{}\n",
        "(".repeat(depth),
        ")".repeat(depth)
    ))
}

/// `count` sibling statements in one block.
pub fn wide_block(count: usize) -> String {
    function(&"\tx = x + 1\n".repeat(count))
}

/// One expression with `count` operands: `x + x + ... + x`.
pub fn long_chain(count: usize) -> String {
    assert!(count > 0, "a chain needs an operand");
    function(&format!("\tx = x{}\n", " + x".repeat(count - 1)))
}

/// `count` branches that each join again, so `2^count` paths reach the end.
pub fn diamonds(count: usize) -> String {
    function(&"\tif x > 0 {\n\t\tx = x - 1\n\t} else {\n\t\tx = x + 1\n\t}\n".repeat(count))
}

fn function(body: &str) -> String {
    let source = format!("package stress\n\nfunc f(x int) int {{\n{body}\treturn x\n}}\n");
    assert!(
        source.len() <= MAX_BYTES,
        "generated source is {} bytes, over the {MAX_BYTES} byte bound",
        source.len()
    );
    source
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn deep_expr_nests_one_operand() {
        assert_eq!(
            deep_expr(3),
            "package stress\n\nfunc f(x int) int {\n\tx = (((x)))\n\treturn x\n}\n"
        );
    }

    #[test]
    fn wide_block_repeats_a_statement() {
        assert_eq!(wide_block(1_000).matches("x = x + 1\n").count(), 1_000);
    }

    #[test]
    fn long_chain_has_the_asked_operands() {
        assert!(long_chain(3).contains("\tx = x + x + x\n"));
        assert!(long_chain(1).contains("\tx = x\n"));
    }

    #[test]
    fn diamonds_branch_and_join() {
        let source = diamonds(2);
        assert_eq!(source.matches("if x > 0 {").count(), 2);
        assert_eq!(source.matches("} else {").count(), 2);
    }

    #[test]
    fn sources_grow_linearly_with_the_request() {
        for generate in [deep_expr, wide_block, long_chain, diamonds] {
            let growth = generate(2_000).len() - generate(1_000).len();
            assert_eq!(generate(3_000).len() - generate(2_000).len(), growth);
        }
    }

    #[test]
    #[should_panic(expected = "byte bound")]
    fn an_oversized_request_is_refused() {
        wide_block(MAX_BYTES);
    }
}
