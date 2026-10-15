//! `library/big_number` 演示（Rust 版）：阶乘、斐波那契、幂、gcd / lcm、同余幂、
//! 字符串互转，以及 `BigFloat` 的十进制浮点（`0.1 + 0.2 == 0.3`、1/3、sqrt(2)、
//! 大整数精确输出）。
//!
//! 输出与 C++ 版 `library/big_number/main.cpp` **逐行一致**（`make check` 跑的就是
//! 那个 C++ 演示），所以两版的输出可以直接 `diff`：
//!
//! ```text
//! cargo run --manifest-path library/big_number-rust/Cargo.toml
//! ```

use big_number::{BigFloat, BigInt};

/// `n!`（迭代，全部用大整数乘）。
fn factorial(n: u32) -> BigInt {
    let mut result = BigInt::from(1);
    for value in 2..=n {
        result *= BigInt::from(value as i64);
    }
    result
}

/// 第 `n` 个斐波那契数（`F(0) = 0`、`F(1) = 1`）。
fn fibonacci(n: u32) -> BigInt {
    let mut a = BigInt::from(0);
    let mut b = BigInt::from(1);
    for _ in 0..n {
        let next = &a + &b;
        a = std::mem::replace(&mut b, next);
    }
    a
}

/// `dec("…")` ↔ C++ 演示里的 `BigFloat("…")`。
fn dec(text: &str) -> BigFloat {
    BigFloat::from_string(text).expect("演示里的文本都是合法的")
}

fn main() {
    println!("2^100      = {}", BigInt::from(2).pow(100));
    println!("30!        = {}", factorial(30));
    println!("F(200)     = {}", fibonacci(200));

    let a: BigInt = "123456789012345678901234567890".parse().expect("合法");
    let b: BigInt = "98765432109876543210".parse().expect("合法");
    println!("a           = {a}");
    println!("b           = {b}");
    println!("a / b       = {}", &a / &b);
    println!("a % b       = {}", &a % &b);
    println!("gcd(a, b)   = {}", BigInt::gcd(&a, &b));
    println!("lcm(12, 18) = {}", BigInt::lcm(&BigInt::from(12), &BigInt::from(18)));
    println!(
        "7^300 mod 1000000007 = {}",
        BigInt::from(7).powmod(300, &BigInt::from(1000000007))
    );
    // 这一行的文案与 C++ 版逐字一致（原文少了「a 的」，为了能直接 diff 就不改了）
    println!("位十进制位数 = {}（limb {} 个）", a.decimal_digits(), a.limb_count());

    println!("\nBigFloat（尾数 × 10^指数，十进制浮点）：");
    let tenth = BigFloat::from_double(0.1);
    let fifth = BigFloat::from_double(0.2);
    println!(
        "0.1 + 0.2    = {}（同样的 double 在二进制浮点里是 0.30000000000000004）",
        tenth + fifth
    );
    println!("1 / 3 (30 位)= {}", (dec("1") / dec("3")).to_string_with_digits(30));
    println!("sqrt(2) (30) = {}", dec("2").sqrt(30).to_string_with_digits(30));
    println!(
        "1e20 + 1     = {}（精确，没有丢低位）",
        (dec("1e20") + dec("1")).to_plain_string()
    );
    let two_pow_200 = BigFloat::from_int(&BigInt::from(2).pow(200));
    println!("2^200 精确   = {}", two_pow_200.to_plain_string());
    println!("2^200 17 位  = {two_pow_200}");
}
