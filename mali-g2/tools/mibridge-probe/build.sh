#!/bin/sh
# Builds mbprobe.dex next to this script: javac (JDK 11+) and D8 from r8.jar
# (https://maven.google.com/com/android/tools/r8/8.5.35/r8-8.5.35.jar), path in $R8_JAR.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
: "${R8_JAR:?set R8_JAR to an r8.jar}"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
javac --release 11 -d "$out" "$here/MbProbe.java"
java -cp "$R8_JAR" com.android.tools.r8.D8 --min-api 31 --output "$out" "$out"/MbProbe.class
mv "$out/classes.dex" "$here/mbprobe.dex"
echo "$here/mbprobe.dex"
