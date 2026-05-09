Import("env")
import sys

if sys.platform == "win32":
    env.Append(LIBS=["winusb", "setupapi", "ws2_32"])
