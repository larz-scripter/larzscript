# -*- coding: utf-8 -*-
"""Larzscript error types."""


class LarzScriptError(Exception):
    """Base class for all Larzscript errors."""


class LarzSyntaxError(LarzScriptError):
    """The source could not be tokenized or parsed."""


class LarzRuntimeError(LarzScriptError):
    """An error while running a program."""


class LarzNameError(LarzRuntimeError):
    """A name was used before it was defined."""


class LarzTypeError(LarzRuntimeError):
    """An operation was applied to the wrong kind of value."""


class MoneyError(LarzRuntimeError):
    """A money operation failed (e.g. a wallet with insufficient funds)."""


class RequireError(LarzRuntimeError):
    """A ``require`` condition was not met - the money-native guardrail."""


class OutOfGasError(LarzRuntimeError):
    """A metered function exceeded the gas budget."""


class SettlementError(LarzRuntimeError):
    """A settlement backend declined or failed to settle a money movement."""
