"""larzscript test suite - pure stdlib unittest, zero dependencies.

Runs real .lz programs and checks money, wallets, the ledger, gas metering,
and the money-native guardrails.
"""
import unittest
from larzscript import (run, Money, Wallet, MoneyError, RequireError,
                        OutOfGasError, LarzSyntaxError, LarzNameError,
                        LarzTypeError)


class TestBasics(unittest.TestCase):
    def test_arithmetic_and_print(self):
        r = run('print(2 + 3 * 4)')
        self.assertEqual(r.output, "14")

    def test_let_and_assign(self):
        r = run('let x = 5\nx = x + 1\nprint(x)')
        self.assertEqual(r.output, "6")
        self.assertEqual(r.get("x"), 6)

    def test_strings_and_concat(self):
        self.assertEqual(run('print("a" + "b" + "c")').output, "abc")

    def test_booleans_and_logic(self):
        self.assertEqual(run('print(true and not false)').output, "true")
        self.assertEqual(run('print(1 < 2 and 3 >= 3)').output, "true")

    def test_if_else(self):
        src = 'let x = 10\nif x > 5 { print("big") } else { print("small") }'
        self.assertEqual(run(src).output, "big")

    def test_while(self):
        src = 'let i = 0\nlet s = 0\nwhile i < 4 { s = s + i\ni = i + 1 }\nprint(s)'
        self.assertEqual(run(src).output, "6")   # 0+1+2+3

    def test_functions_and_return(self):
        src = 'fn add(a, b) { return a + b }\nprint(add(2, 40))'
        self.assertEqual(run(src).output, "42")

    def test_recursion(self):
        src = ('fn fact(n) { if n <= 1 { return 1 }\nreturn n * fact(n - 1) }\n'
               'print(fact(5))')
        self.assertEqual(run(src).output, "120")


class TestMoney(unittest.TestCase):
    def test_money_literal_and_format(self):
        r = run('print($3.50)\nprint($9)\nprint($0.02)')
        self.assertEqual(r.output, "$3.50\n$9.00\n$0.02")

    def test_money_arithmetic(self):
        r = run('print($1.50 + $2.00)\nprint($10.00 - $3.50)\nprint($2.00 * 3)')
        self.assertEqual(r.output, "$3.50\n$6.50\n$6.00")

    def test_money_split(self):
        # 80% of $9.00 = $7.20 ; money divided by a number stays money
        self.assertEqual(run('print($9.00 * 0.8)').output, "$7.20")

    def test_money_is_its_own_type(self):
        self.assertRaises(LarzTypeError, run, 'print($5 + 3)')     # money + number
        self.assertRaises(LarzTypeError, run, 'print($5 < 3)')     # money vs number


class TestWalletsAndPay(unittest.TestCase):
    def test_pay_moves_money_and_records_ledger(self):
        src = ('wallet customer = $10.00\nwallet shop\nprice coffee = $3.50\n'
               'pay coffee from customer to shop')
        r = run(src)
        self.assertEqual(r.get("customer").balance, Money(650))
        self.assertEqual(r.get("shop").balance, Money(350))
        self.assertEqual(len(r.ledger), 1)
        txn = r.ledger[0]
        self.assertEqual((txn.src, txn.dst), ("customer", "shop"))
        self.assertEqual(txn.amount, Money(350))

    def test_insufficient_funds_raises(self):
        src = 'wallet a = $1.00\nwallet b\npay $5.00 from a to b'
        self.assertRaises(MoneyError, run, src)

    def test_credit_and_debit_methods(self):
        src = 'wallet w\nw.credit($5.00)\nw.debit($2.00)\nprint(w.balance)'
        self.assertEqual(run(src).output, "$3.00")

    def test_revenue_split_program(self):
        src = ('wallet customer = $20.00\nwallet platform\nwallet creator\n'
               'price premium = $9.00\n'
               'fn buy(b) { pay premium from b to platform\n'
               '            pay premium * 0.8 from platform to creator }\n'
               'buy(customer)')
        r = run(src)
        self.assertEqual(r.get("creator").balance, Money(720))    # $7.20
        self.assertEqual(r.get("platform").balance, Money(180))   # $1.80
        self.assertEqual(len(r.ledger), 2)


class TestRequire(unittest.TestCase):
    def test_require_passes(self):
        run('wallet w = $10.00\nrequire w.balance >= $5.00, "need 5"')

    def test_require_fails_with_message(self):
        try:
            run('wallet w = $1.00\nrequire w.balance >= $5.00, "need five dollars"')
            self.fail("should have raised")
        except RequireError as e:
            self.assertIn("need five dollars", str(e))

    def test_require_no_message(self):
        self.assertRaises(RequireError, run, 'require false')


class TestGasMetering(unittest.TestCase):
    def test_gas_used_counted(self):
        src = 'fn f() gas 300 { return 1 }\nf()\nf()'
        r = run(src)
        self.assertEqual(r.gas_used, 600)

    def test_out_of_gas(self):
        # budget 1200 allows two 500-gas calls, not a third
        src = 'fn scan() gas 500 { return 1 }\nscan()\nscan()\nscan()'
        self.assertRaises(OutOfGasError, run, src, 1200)

    def test_within_budget_ok(self):
        src = 'fn scan() gas 500 { return 1 }\nscan()\nscan()'
        r = run(src, gas=1200)
        self.assertEqual(r.gas_used, 1000)


class TestErrors(unittest.TestCase):
    def test_syntax_error(self):
        self.assertRaises(LarzSyntaxError, run, 'let = 5')
        self.assertRaises(LarzSyntaxError, run, 'pay $5 from a')   # missing 'to'

    def test_name_error(self):
        self.assertRaises(LarzNameError, run, 'print(nope)')

    def test_type_error_pay_nonwallet(self):
        self.assertRaises(LarzTypeError, run, 'let a = 5\nwallet b\npay $1 from a to b')


if __name__ == "__main__":
    unittest.main()


class TestSubscriptions(unittest.TestCase):
    def test_subscribe_charges_and_grants(self):
        src = ('wallet customer = $20.00\nwallet platform\n'
               'paywall pro = $9.00 / month to platform\n'
               'subscribe customer to pro\n'
               'print("has pro:", customer has pro)')
        r = run(src)
        self.assertEqual(r.get("customer").balance, Money(1100))   # $11.00 left
        self.assertEqual(r.get("platform").balance, Money(900))    # $9.00
        self.assertEqual(r.output, "has pro: true")
        self.assertEqual(len(r.ledger), 1)

    def test_has_is_false_before_subscribing(self):
        src = ('wallet customer = $20.00\nwallet platform\n'
               'paywall pro = $9.00 / month to platform\n'
               'print(customer has pro)')
        self.assertEqual(run(src).output, "false")

    def test_require_access_gate(self):
        src = ('wallet customer = $20.00\nwallet platform\n'
               'paywall pro = $9.00 / month to platform\n'
               'fn premium_feature(user) {\n'
               '    require user has pro, "subscribe to pro first"\n'
               '    return "secret content"\n'
               '}\n'
               'premium_feature(customer)')
        try:
            run(src)
            self.fail("should have been gated")
        except RequireError as e:
            self.assertIn("subscribe to pro first", str(e))

    def test_gate_passes_after_subscribe(self):
        src = ('wallet customer = $20.00\nwallet platform\n'
               'paywall pro = $9.00 / month to platform\n'
               'subscribe customer to pro\n'
               'fn premium_feature(user) {\n'
               '    require user has pro, "subscribe first"\n'
               '    return "secret content"\n'
               '}\n'
               'print(premium_feature(customer))')
        self.assertEqual(run(src).output, "secret content")

    def test_cannot_subscribe_without_funds(self):
        src = ('wallet customer = $1.00\nwallet platform\n'
               'paywall pro = $9.00 / month to platform\n'
               'subscribe customer to pro')
        self.assertRaises(MoneyError, run, src)


class TestBuiltins(unittest.TestCase):
    def test_len(self):
        self.assertEqual(run('print(len("hello"))').output, "5")

    def test_money_builtin(self):
        self.assertEqual(run('print(money(3.5))').output, "$3.50")
