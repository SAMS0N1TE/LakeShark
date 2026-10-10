"""Compile the patched SDK's real SPI setup/cleanup functions with fault injection."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--idf", default="C:/esp/v5.5.4/esp-idf")
    sdk = Path(parser.parse_args().idf)
    source = (sdk / "components/esp_driver_spi/src/gpspi/spi_master.c").read_text()
    start = source.index("static SPI_MASTER_ISR_ATTR void uninstall_priv_desc")
    end = source.index("esp_err_t SPI_MASTER_ATTR spi_device_queue_trans", start)
    with tempfile.TemporaryDirectory(prefix="ls-spi-dma-") as folder:
        root = Path(folder)
        (root / "spi_under_test.h").write_text(source[start:end])
        exe = root / "test.exe"
        compiler = shutil.which("gcc")
        if not compiler:
            raise RuntimeError("host gcc required")
        subprocess.run([compiler, "-std=c11", "-Wno-pointer-to-int-cast", "-I", str(root),
                        str(Path(__file__).with_suffix(".c")), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
