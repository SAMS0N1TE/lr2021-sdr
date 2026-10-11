"""Package the default ESP32-P4 build; requires only Python's standard library."""
import hashlib
import os
from pathlib import Path
import zipfile

root = Path(__file__).resolve().parents[1]
fw = root / "firmware/esp32p4"
build = fw / "build"
files = {
    "lr2021_capture.bin": build / "lr2021_capture.bin",
    "bootloader.bin": build / "bootloader/bootloader.bin",
    "partition-table.bin": build / "partition_table/partition-table.bin",
    "README.md": fw / "README.md",
    "LICENSE": root / "LICENSE",
}
contents = {name: path.read_bytes() for name, path in files.items()}
idf = Path(os.environ["IDF_PATH"])
licenses = {
    "ESP-IDF.txt": "LICENSE",
    "FreeRTOS.txt": "components/freertos/FreeRTOS-Kernel/LICENSE.md",
    "newlib.txt": "components/newlib/COPYING.NEWLIB",
    "picolibc.txt": "components/newlib/COPYING.picolibc",
    "micro-ecc.txt": "components/bootloader/subproject/components/micro-ecc/micro-ecc/LICENSE.txt",
}
for name, path in licenses.items():
    contents["licenses/" + name] = (idf / path).read_bytes()
sums = "".join(f"{hashlib.sha256(data).hexdigest()}  {name}\n"
               for name, data in contents.items() if name.endswith(".bin"))
out = build / "lr2021-sdr-esp32p4.zip"
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as archive:
    for name, data in contents.items():
        archive.writestr(name, data)
    archive.writestr("SHA256SUMS", sums)
print(out)
