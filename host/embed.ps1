# Build host\apache.rex into the host as a C++ raw string literal, as the
# Makefile does on Unix: build\apache_src.h. Line ends become LF, whatever
# the checkout has. MSVC takes no string literal longer than 16380 bytes.
#
#   powershell -File host\embed.ps1 host\apache.rex build\apache_src.h
#
# Copyright 2026 Josep Maria Blasco
# Licensed under the Apache License, Version 2.0 (see LICENSE).
param([string]$src, [string]$dst)
$t = [IO.File]::ReadAllText($src) -replace "`r`n", "`n"
if ($t.Length -gt 16000) {
    Write-Error "$src is too long for one MSVC string literal"
    exit 1
}
$q = [char]34
[IO.File]::WriteAllText($dst, "static const char apache_src[] = R${q}orx_rexx(`n" + $t + ")orx_rexx${q};`n")
