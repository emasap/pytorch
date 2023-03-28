import contextlib
import torch
from torch._C._functorch import (
    set_single_level_autograd_function_allowed,
    get_single_level_autograd_function_allowed,
    unwrap_if_dead,
)

@contextlib.contextmanager
def enable_single_level_autograd_function():
    try:
        prev_state = get_single_level_autograd_function_allowed()
        set_single_level_autograd_function_allowed(True)
        yield
    finally:
        set_single_level_autograd_function_allowed(prev_state)

def unwrap_dead_wrappers(args):
    # NB: doesn't use tree_map_only for performance reasons
    result = tuple(
        unwrap_if_dead(arg) if isinstance(arg, torch.Tensor) else arg
        for arg in args
    )
    return result
