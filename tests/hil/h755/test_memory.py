import pytest
from memory_cases import check_memory_policy

pytestmark = pytest.mark.hil


def test_no_heap_and_stack_watermark(board):
    check_memory_policy(board)
