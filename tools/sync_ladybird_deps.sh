#!/bin/bash
# sync_ladybird_deps.sh — Recursive dependency resolver for TaterTOS port

UPSTREAM="/home/legacyindieradio/ladybird-upstream/Libraries"
PORT="src/user/apps/ladybird/Libraries"

echo "Checking for missing headers and implementations in $PORT (Excluding Bindings)..."

iteration=0
while true; do
    iteration=$((iteration + 1))
    echo "Iteration $iteration..."
    
    # Find all Lib... includes in the current port
    MISSING=$(find "$PORT" -name "*.h" -o -name "*.cpp" | xargs grep -h "#include <Lib" | \
              sed -E 's/.*<([^>]*)>.*/\1/' | sort | uniq)
    
    COPIED=0
    for inc in $MISSING; do
        # SKIP BINDINGS - We handle them via manual stubs to avoid generator vortex
        if [[ "$inc" == *"Bindings/"* ]]; then
            continue
        fi

        # 1. Pull the header if missing
        if [ ! -f "$PORT/$inc" ]; then
            if [ -f "$UPSTREAM/$inc" ]; then
                DIR=$(dirname "$inc")
                mkdir -p "$PORT/$DIR"
                cp "$UPSTREAM/$inc" "$PORT/$inc"
                echo "  + Header: $inc"
                COPIED=$((COPIED + 1))
            fi
        fi
        
        # 2. Pull the implementation (.cpp) if it exists and we don't have it
        cpp="${inc%.h}.cpp"
        if [[ "$inc" == *.h ]] && [ -f "$UPSTREAM/$cpp" ] && [ ! -f "$PORT/$cpp" ]; then
             DIR=$(dirname "$cpp")
             mkdir -p "$PORT/$DIR"
             cp "$UPSTREAM/$cpp" "$PORT/$cpp"
             echo "  + Impl:   $cpp"
             COPIED=$((COPIED + 1))
        fi
    done
    
    if [ "$COPIED" -eq 0 ]; then
        echo "No more missing files found in upstream."
        break
    fi
    
    if [ "$iteration" -gt 20 ]; then
        echo "Safety break: reached 20 iterations."
        break
    fi
done
