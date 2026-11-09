#!/bin/bash
# Build the polyfill layer into polyfill/build:
#   libwineskin_polyfill.a  every src/*.c and src/objc/*.m, for i386 and x86_64, hidden visibility: an
#                           image that links a member holds a private copy and exports nothing
#   include/                system framework headers regenerated with the declarations a later SDK gives them
#   Frameworks/             stub frameworks for libraries 10.9 does not have, to link weakly against
#   link-<arch>.rsp         the linker arguments every Wine host link takes (a clang response file)
# include/ (committed) is the hand-written header overlay; both include directories are on every Wine
# host compile's system include path.
#
# build.sh runs this before every Wine build. An output is replaced only when its bytes change: the Wine
# trees depend on these files by modification time (scripts/track-polyfill.sh).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/../scripts/build-log.sh"
build_log_open
. "$HERE/../scripts/env.sh"
OUT="$HERE/build"
# ccache (CCACHE_BASEDIR) hands the compiler paths relative to the current directory, and a member's
# __FILE__ strings keep them: compiling from one directory keeps a member's bytes the same.
cd "$HERE"
mkdir -p "$OUT/obj" "$OUT/include" "$OUT/Frameworks"

update() {  # <new> <file>: <new> becomes <file> if their bytes differ, else <file> keeps its time
    if cmp -s "$1" "$2"; then rm -f "$1"; else mv -f "$1" "$2"; fi
}

echo "### polyfill: framework headers"
# The 10.11 SDK declares Foundation's collection classes with lightweight generics, and later SDKs give
# some AppKit accessors the types they return. These are annotations for the compiler; the classes and
# methods are 10.9's.
FRAMEWORKS="$WS_SYSROOT/System/Library/Frameworks"
GEN="$OUT/include.next"
rm -rf "$GEN"
mkdir -p "$GEN/Foundation" "$GEN/AppKit"
sdk_copy() {  # <framework> <header>: the generated copy of a framework header
    local h="$GEN/$1/$2"
    [ -f "$h" ] || cp "$FRAMEWORKS/$1.framework/Headers/$2" "$h"
    echo "$h"
}
generic() {  # <header> <class> <parameter list>: parameterize the class and its categories
    local h names
    h="$(sdk_copy Foundation "$1")"
    names="$(echo "$3" | sed -E 's/__covariant //g')"
    sed -i '' -E -e "s/^@interface $2 :/@interface $2<$3> :/" -e "s/^@interface $2 \(/@interface $2<$names> (/" "$h"
    grep -q "^@interface $2<" "$h" || { echo "no @interface $2 in $1" >&2; exit 1; }
}
retype() {  # <framework> <header> <selector start> <type>: the type a later SDK gives a method's result
    local h
    h="$(sdk_copy "$1" "$2")"
    grep -qE "^- \(id\)$3([: ;]|\$)" "$h" || { echo "no -(id)$3 in $2" >&2; exit 1; }
    sed -i '' -E "s/^- \(id\)$3([: ;]|\$)/- ($4)$3\1/" "$h"
}
generic NSArray.h      NSArray             "__covariant ObjectType"
generic NSArray.h      NSMutableArray      "ObjectType"
generic NSDictionary.h NSDictionary        "__covariant KeyType, __covariant ObjectType"
generic NSDictionary.h NSMutableDictionary "KeyType, ObjectType"
generic NSSet.h        NSSet               "__covariant ObjectType"
generic NSSet.h        NSMutableSet        "ObjectType"
generic NSSet.h        NSCountedSet        "ObjectType"
generic NSOrderedSet.h NSOrderedSet        "__covariant ObjectType"
generic NSOrderedSet.h NSMutableOrderedSet "ObjectType"
generic NSEnumerator.h NSEnumerator        "__covariant ObjectType"
generic NSHashTable.h  NSHashTable         "ObjectType"
generic NSMapTable.h   NSMapTable          "KeyType, ObjectType"
generic NSCache.h      NSCache             "KeyType, ObjectType"
for sel in objectAtIndex firstObject lastObject objectAtIndexedSubscript; do
    retype Foundation NSArray.h "$sel" ObjectType
    retype Foundation NSOrderedSet.h "$sel" ObjectType
done
retype Foundation NSDictionary.h objectForKey ObjectType
retype Foundation NSDictionary.h objectForKeyedSubscript ObjectType
retype Foundation NSSet.h member ObjectType
retype Foundation NSSet.h anyObject ObjectType
retype Foundation NSEnumerator.h nextObject ObjectType
retype AppKit NSWindow.h contentView "__kindof NSView *"
# Changed headers replace their copies; a header no longer generated goes.
rsync -rc --delete "$GEN/" "$OUT/include/"
rm -rf "$GEN"

echo "### polyfill: archive"
# Every member is compiled on every run (ccache makes that cheap) and replaces the old one only if it
# changed, so a member's time is the time its code last changed.
for arch in i386 x86_64; do
    mkdir -p "$OUT/obj/$arch"
    CF="$(ws_host_cflags $arch) -fPIC -fvisibility=hidden -O2 -Wall -isystem $OUT/include -isystem $HERE/include"
    members=()
    for f in "$HERE"/src/*.c "$HERE"/src/objc/*.m; do
        case "$f" in
            *.m) o="$OUT/obj/$arch/objc-$(basename "${f%.m}").o" ;;
            *)   o="$OUT/obj/$arch/$(basename "${f%.c}").o" ;;
        esac
        $WS_CCACHE "$WS_HOST_CC" $CF -c "$f" -o "$o.next"
        update "$o.next" "$o"
        members+=("$o")
    done
    for o in "$OUT/obj/$arch"/*.o; do  # members whose source is gone
        case " ${members[*]} " in *" $o "*) ;; *) rm -f "$o" ;; esac
    done
    rm -f "$OUT/obj/$arch.a"
    llvm-ar rcs "$OUT/obj/$arch.a" "$OUT/obj/$arch"/*.o
done
lipo -create "$OUT/obj/i386.a" "$OUT/obj/x86_64.a" -output "$OUT/libwineskin_polyfill.a.next"
update "$OUT/libwineskin_polyfill.a.next" "$OUT/libwineskin_polyfill.a"

echo "### polyfill: stub frameworks"
# Metal.framework, for a link naming it. Images take its functions from the archive and link it weakly
# (link-<arch>.rsp), so 10.9's dyld skips it.
FW="$OUT/Frameworks/Metal.framework"
mkdir -p "$FW/Versions/A"
: > "$OUT/obj/Metal.c"
for arch in i386 x86_64; do
    "$WS_HOST_CC" $(ws_host_cflags $arch) $(ws_host_ldflags $arch) -dynamiclib \
        -install_name /System/Library/Frameworks/Metal.framework/Versions/A/Metal \
        "$OUT/obj/Metal.c" -o "$OUT/obj/Metal.$arch"
done
lipo -create "$OUT/obj/Metal.i386" "$OUT/obj/Metal.x86_64" -output "$FW/Versions/A/Metal.next"
update "$FW/Versions/A/Metal.next" "$FW/Versions/A/Metal"
[ "$(readlink "$FW/Versions/Current")" = A ] || ln -sfh A "$FW/Versions/Current"
[ "$(readlink "$FW/Metal")" = Versions/Current/Metal ] || ln -sfh Versions/Current/Metal "$FW/Metal"

echo "### polyfill: link arguments"
# Classes 10.9's frameworks lack are weak and bound by name at load (-U): they resolve where a system
# has them and are nil on 10.9. An i386 image names an Objective-C class by the legacy runtime's
# .objc_class_name_ symbol.
for arch in i386 x86_64; do
    case $arch in
        i386)   CLS='.objc_class_name_' ;;
        x86_64) CLS='_OBJC_CLASS_$_' ;;
    esac
    {
        echo "$OUT/libwineskin_polyfill.a"
        echo "-F$OUT/Frameworks"
        echo "-Wl,-weak_framework,Metal"
        echo "-Wl,-U,${CLS}CAMetalLayer"
    } > "$OUT/link-$arch.rsp.next"
    update "$OUT/link-$arch.rsp.next" "$OUT/link-$arch.rsp"
done
echo "### polyfill OK -> $OUT"
