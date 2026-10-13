# Generate the real mapping for this artifact without enabling release optimization.
# R8 still performs the bytecode/desugaring work required for Android DEX output.
-dontobfuscate
-dontshrink
-dontoptimize

# Retain source/line, annotation, signature and enclosing-class debug attributes.
-keepattributes *
