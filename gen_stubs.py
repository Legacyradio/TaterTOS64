import re

with open("skia_errors.txt", "r") as f:
    text = f.read()

symbols = set(re.findall(r"undefined reference to `([^']+)'", text))
if not symbols:
    print("No undefined references found!")
    exit(0)

# Filter out non-mangled and C functions we can't easily guess.
# Actually, the linker output will now have mangled names like `_Z...` or `Fc...` or `__...`.
# We can safely use all of them as assembly labels!
asm_stubs = "\n// --- AUTO-GENERATED ASSEMBLY STUBS ---\n"
for s in sorted(symbols):
    # Only alphanumeric and _ are valid in unquoted asm labels.
    if re.match(r'^[a-zA-Z_][a-zA-Z0-9_]*$', s):
        asm_stubs += f'__asm__(".global {s}\\n{s}:\\n");\n'
    else:
        print(f"Skipping invalid asm symbol: {s}")

with open("src/user/apps/ladybird/test/web_smoke_stubs.cpp", "a") as f:
    f.write(asm_stubs)
    
print(f"Appended {len(symbols)} assembly stubs to web_smoke_stubs.cpp.")
