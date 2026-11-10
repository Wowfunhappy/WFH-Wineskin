#!/bin/bash

# WINESKIN: part of WFH Wineskin, not of Wine.
#
# Update wrappers made from WFH Wineskin.app to the engine and launcher of this build, preserving each
# wrapper's Windows environment, settings, name and icon. Every wrapper carries its own engine; this
# replaces that copy, then has Wine update the wrapper's prefix, which refreshes the files a prefix
# copies from its engine (the visual style, placeholder DLLs, registry defaults).
#
#   Update Existing Wrappers.command                        choose wrappers in a dialog
#   Update Existing Wrappers.command A.app B.app ...        update the named wrappers
#   Update Existing Wrappers.command --update-apps-folder   update every wrapper in /Applications

set -e
cd "`dirname "$0"`"

template_app="../../build/WFH Wineskin.app"
template_contents="$template_app/Contents"

if [ ! -x "$template_contents/MacOS/Wineskin" ] ||
   [ ! -x "$template_contents/Resources/Engine/bin/wine" ]
then
	echo "Error: Build WFH Wineskin.app (build.sh, then wrapper/build-wrapper.sh) before updating existing wrappers."
	exit 1
fi

selected_apps=()
interactive_mode=false
current_app=

# alert <title> [message] [critical]: an alert whose text reaches AppleScript as arguments, so names
# with quotes in them show as they are.
alert()
{
	osascript -e 'on run argv' \
	          -e 'if item 3 of argv is "critical" then' \
	          -e 'display alert (item 1 of argv) message (item 2 of argv) as critical' \
	          -e 'else' \
	          -e 'display alert (item 1 of argv) message (item 2 of argv)' \
	          -e 'end if' \
	          -e 'end run' "$1" "${2:-}" "${3:-}" > /dev/null
}

# A command that fails stops the script (set -e); in dialog mode, say so instead of quitting silently.
trap 'if [ "$interactive_mode" = true ] && [ -n "$current_app" ]; then alert "$current_app could not be updated." "A step of the update failed. Run this script from Terminal with the wrapper as its argument to see why." critical; fi' ERR

if [ "$1" = "--update-apps-folder" ]
then
	for app in /Applications/*.app
	do
		if [ -f "$app/Contents/SharedSupport/Wineskin.plist" ]
		then
			selected_apps+=("$app")
		fi
	done
elif [ $# -gt 0 ]
then
	for app in "$@"
	do
		selected_apps+=("$app")
	done
else
	interactive_mode=true
	input_apps=$(osascript <<'APPLESCRIPT'
tell application "System Events"
	activate
	set chosenFiles to choose file of type {"com.apple.application-bundle"} with multiple selections allowed with prompt "Select wrappers to update"
	set posixPaths to ""
	repeat with aFile in chosenFiles
		set posixPaths to posixPaths & POSIX path of aFile & linefeed
	end repeat
	if posixPaths is not "" then
		return text 1 thru -2 of posixPaths
	else
		return ""
	end if
end tell
APPLESCRIPT
)
	while IFS= read -r app
	do
		if [ -n "$app" ]
		then
			selected_apps+=("$app")
		fi
	done <<< "$input_apps"
fi

if [ ${#selected_apps[@]} -eq 0 ]
then
	exit 1
fi

updated_apps=()
invalid_apps=()
# What belongs to the wrapper rather than to the template: the Windows environment, settings and
# logs; the icon made from its program; and Info.plist, which carries its name and identifier.
user_items=(
	"SharedSupport"
	"Resources/Wineskin.icns"
	"Info.plist"
)

template_path=$(cd "$template_app" && pwd -P)

for input_app in "${selected_apps[@]}"
do
	if [ ! -d "$input_app/Contents" ]
	then
		invalid_apps+=("$(basename "$input_app")")
		continue
	fi

	current_app=$(basename "$input_app")
	input_path=$(cd "$input_app" && pwd -P)
	app_contents="$input_path/Contents"
	if [ "$input_path" = "$template_path" ] ||
	   [ ! -x "$app_contents/MacOS/Wineskin" ] ||
	   [ ! -f "$app_contents/Info.plist" ] ||
	   [ ! -f "$app_contents/SharedSupport/Wineskin.plist" ]
	then
		invalid_apps+=("$(basename "$input_app")")
		continue
	fi

	prefix="$app_contents/SharedSupport/prefix"
	engine="$app_contents/Resources/Engine"

	# Nothing of the wrapper may be running while its engine is replaced.
	if [ -d "$prefix" ] && [ -x "$engine/bin/wineserver" ]
	then
		WINEPREFIX="$prefix" "$engine/bin/wineserver" -k 2>/dev/null || true
		WINEPREFIX="$prefix" "$engine/bin/wineserver" -w 2>/dev/null || true
	fi

	# An engine that is a link to another one becomes the wrapper's own copy.
	if [ -L "$engine" ]
	then
		rm "$engine"
	fi

	exclude_options=()
	for item in "${user_items[@]}"
	do
		exclude_options+=("--exclude=/$item")
	done

	rsync -a --delete "${exclude_options[@]}" "$template_contents/" "$app_contents/"

	# Info.plist is the template's with the wrapper's own name and identifier (a wrapper with no
	# program has none; the wrapper sets it from its program at its next launch).
	name=$(/usr/libexec/PlistBuddy -c "Print :CFBundleName" "$app_contents/Info.plist")
	identifier=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$app_contents/Info.plist" 2>/dev/null || true)
	cp "$template_contents/Info.plist" "$app_contents/Info.plist"
	# plutil takes the values as they are; PlistBuddy's command parser stops at quotes (Baldur's Gate).
	plutil -replace CFBundleName -string "$name" "$app_contents/Info.plist"
	if [ -n "$identifier" ]
	then
		plutil -replace CFBundleIdentifier -string "$identifier" "$app_contents/Info.plist"
	fi

	if [ -d "$prefix" ]
	then
		WINEPREFIX="$prefix" WINEDEBUG=-all DYLD_FALLBACK_LIBRARY_PATH="$engine/lib:/usr/lib" \
			"$engine/bin/wine" wineboot --update
		WINEPREFIX="$prefix" "$engine/bin/wineserver" -w
	fi

	touch "$app_contents/Resources/Wineskin.icns"
	touch "$input_path"
	updated_apps+=("$(basename "$input_app")")
	current_app=
done

if [ "$interactive_mode" = true ]
then
	if [ ${#invalid_apps[@]} -eq 1 ]
	then
		alert "${invalid_apps[0]} was not modified because it is not a WFH Wineskin wrapper." "" critical
	elif [ ${#invalid_apps[@]} -gt 1 ]
	then
		alert "These apps were not modified because they are not WFH Wineskin wrappers:" "$(printf "%s\n" "${invalid_apps[@]}")" critical
	fi

	if [ ${#updated_apps[@]} -eq 1 ]
	then
		alert "${updated_apps[0]} updated successfully."
	elif [ ${#updated_apps[@]} -gt 1 ]
	then
		alert "Wrappers updated successfully:" "$(printf "%s\n" "${updated_apps[@]}")"
	fi
else
	printf "Updated: %s\n" "${updated_apps[@]}"
	if [ ${#invalid_apps[@]} -gt 0 ]
	then
		printf "Skipped: %s\n" "${invalid_apps[@]}"
	fi
fi
