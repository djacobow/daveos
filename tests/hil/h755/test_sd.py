"""H755 SPI SD cases; shared read-only and explicitly opted-in write tests."""
from sd_cases import (spi_board, test_read_only_sd_inspection,
                      test_read_only_filesystem, test_create_file_opt_in, test_sd_update_opt_in, test_sd_foreign_update_opt_in)
import pytest

pytestmark = pytest.mark.hil
