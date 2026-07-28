"""Parity tests: every program must produce IDENTICAL results on both the
tree-walking interpreter and the bytecode VM. This is what proves the compiler
is correct - the two backends agree on output, ledger, balances and gas."""
import unittest
from larzscript import run, Money, MoneyError, RequireError, OutOfGasError


# Programs exercised on BOTH backends. Each: (source, gas_or_None)
PROGRAMS = [
    ('print(2 + 3 * 4)', None),
    ('let x = 5\nx = x + 1\nprint(x)', None),
    ('print("a" + "b" + "c")', None),
    ('print(true and not false)\nprint(1 < 2 or 5 < 1)', None),
    ('let x = 10\nif x > 5 { print("big") } else { print("small") }', None),
    ('let i = 0\nlet s = 0\nwhile i < 5 { s = s + i\ni = i + 1 }\nprint(s)', None),
    ('fn add(a, b) { return a + b }\nprint(add(2, 40))', None),
    ('fn fact(n) { if n <= 1 { return 1 }\nreturn n * fact(n - 1) }\nprint(fact(6))', None),
    ('print($3.50 + $2.00)\nprint($9.00 * 0.8)', None),
    ('wallet customer = $10.00\nwallet shop\nprice coffee = $3.50\n'
     'pay coffee from customer to shop\nprint(customer.balance, shop.balance)', None),
    ('wallet c = $20.00\nwallet p\nwallet creator\nprice premium = $9.00\n'
     'fn buy(b) { pay premium from b to p\npay premium * 0.8 from p to creator }\n'
     'buy(c)\nprint(creator.balance)', None),
    ('wallet w\nw.credit($5.00)\nw.debit($2.00)\nprint(w.balance)', None),
    ('wallet u = $5.00\nwallet api\nprice per = $0.02\n'
     'fn scan(x) gas 500 { pay per from u to api\nreturn "ok" }\n'
     'print(scan("a"))\nprint(scan("b"))\nprint(api.balance)', 1200),
    ('wallet customer = $20.00\nwallet platform\n'
     'paywall pro = $9.00 / month to platform\nsubscribe customer to pro\n'
     'fn feat(user) { require user has pro, "no"\nreturn "content" }\n'
     'print(feat(customer))\nprint(customer.balance)', None),
    ('print(len("hello"))\nprint(money(3.5))', None),
]


class TestBackendParity(unittest.TestCase):
    def test_identical_results(self):
        for src, gas in PROGRAMS:
            tree = run(src, gas=gas, backend="tree")
            vm = run(src, gas=gas, backend="vm")
            self.assertEqual(tree.output, vm.output, "output differs for:\n%s" % src)
            self.assertEqual(tree.gas_used, vm.gas_used, "gas differs for:\n%s" % src)
            # ledger equality by (src, dst, cents)
            tl = [(t.src, t.dst, t.amount.cents) for t in tree.ledger]
            vl = [(t.src, t.dst, t.amount.cents) for t in vm.ledger]
            self.assertEqual(tl, vl, "ledger differs for:\n%s" % src)


class TestVMErrors(unittest.TestCase):
    def test_vm_insufficient_funds(self):
        src = 'wallet a = $1.00\nwallet b\npay $5.00 from a to b'
        self.assertRaises(MoneyError, run, src, backend="vm")

    def test_vm_require(self):
        self.assertRaises(RequireError, run, 'require false', backend="vm")

    def test_vm_out_of_gas(self):
        src = 'fn f() gas 500 { return 1 }\nf()\nf()\nf()'
        self.assertRaises(OutOfGasError, run, src, gas=1200, backend="vm")

    def test_vm_state_accessible(self):
        src = ('wallet customer = $10.00\nwallet shop\nprice coffee = $3.50\n'
               'pay coffee from customer to shop')
        r = run(src, backend="vm")
        self.assertEqual(r.get("shop").balance, Money(350))
        self.assertEqual(len(r.ledger), 1)

    def test_vm_subscription(self):
        src = ('wallet customer = $20.00\nwallet platform\n'
               'paywall pro = $9.00 / month to platform\nsubscribe customer to pro\n'
               'print(customer has pro)')
        self.assertEqual(run(src, backend="vm").output, "true")


if __name__ == "__main__":
    unittest.main()
