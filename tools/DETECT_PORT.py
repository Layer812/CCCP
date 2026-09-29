import sys
from serial.tools import list_ports

requested = (sys.argv[1] if len(sys.argv) > 1 else "AUTO").strip().upper()
ports = list(list_ports.comports())


def text(p):
    return " | ".join(str(x or "") for x in (
        p.device, p.description, p.manufacturer, p.hwid,
    ))

print("CCCP serial-port probe:", file=sys.stderr)
if not ports:
    print("  no COM ports are currently visible to Windows", file=sys.stderr)
    sys.exit(2)

for p in ports:
    vidpid = ""
    if p.vid is not None or p.pid is not None:
        vidpid = f" VID:PID={p.vid or 0:04X}:{p.pid or 0:04X}"
    print(f"  {p.device}: {p.description or '-'}{vidpid}", file=sys.stderr)

# Keep the traditional COM9 argument as a preference, but do not fail just
# because the ESP32-S3 enumerated on another COM number.
if requested not in ("", "AUTO"):
    for p in ports:
        if p.device.upper() == requested:
            print(f"CCCP port: requested {requested} is present", file=sys.stderr)
            print(p.device)
            sys.exit(0)
    print(f"CCCP port: requested {requested} is not present; trying auto-detect", file=sys.stderr)

ranked = []
for p in ports:
    blob = text(p).upper()
    score = 0
    reasons = []

    if p.vid == 0x303A or "VID:PID=303A:" in blob or "VID_303A" in blob:
        score += 100
        reasons.append("Espressif VID 303A")
    if "ESPRESSIF" in blob:
        score += 40
        reasons.append("Espressif")
    if "USB JTAG/SERIAL" in blob or "USB SERIAL/JTAG" in blob:
        score += 35
        reasons.append("USB Serial/JTAG")
    if "M5STACK" in blob or "M5STACK" in blob:
        score += 30
        reasons.append("M5Stack")
    if "ESP32" in blob:
        score += 20
        reasons.append("ESP32")

    ranked.append((score, p, reasons))

ranked.sort(key=lambda x: (-x[0], x[1].device))
best_score = ranked[0][0]
best = [r for r in ranked if r[0] == best_score]

if best_score > 0 and len(best) == 1:
    score, p, reasons = best[0]
    print(f"CCCP port: auto-selected {p.device} ({', '.join(reasons)})", file=sys.stderr)
    print(p.device)
    sys.exit(0)

# If Windows exposes exactly one serial device, using it is less error-prone
# than forcing COM9, and still avoids any user-side Device Manager lookup.
if len(ports) == 1:
    p = ports[0]
    print(f"CCCP port: only one COM port exists; selected {p.device}", file=sys.stderr)
    print(p.device)
    sys.exit(0)

if best_score > 0 and len(best) > 1:
    print("ERROR: more than one equally likely CCCP/ESP32 port was found.", file=sys.stderr)
else:
    print("ERROR: CCCP could not safely identify the Cardputer ADV port.", file=sys.stderr)
print("Pass the desired COM port explicitly, e.g. R1_FLASH_AUTO.bat COM12", file=sys.stderr)
sys.exit(3)
