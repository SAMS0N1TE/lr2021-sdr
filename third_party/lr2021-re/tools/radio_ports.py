"""Find the attached bench boards even when USB tty numbers change after reboot."""
from serial.tools import list_ports


def default_port(description, fallback):
    matches = [p.device for p in list_ports.comports()
               if description.casefold() in (p.description or '').casefold()]
    return matches[0] if len(matches) == 1 else fallback
