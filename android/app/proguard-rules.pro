# The app has no reflection-based code of its own; keep it whole so stack
# traces in bug reports stay readable.
-keep class org.oldarmemu.** { *; }
-dontobfuscate
