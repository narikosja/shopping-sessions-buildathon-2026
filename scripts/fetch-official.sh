#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
base=https://www.joinbuilders.org/buildathon/tracks/shopping-sessions/files
mkdir -p official
for file in rules.md starter-duckdb.zip starter-polars.zip starter-pyspark.zip generator.zip dev-answer.zip; do
    if [ ! -f "official/$file" ]; then
        curl --fail --silent --show-error --location --retry 3 "$base/$file" -o "official/$file.tmp"
        mv "official/$file.tmp" "official/$file"
    fi
    case "$file" in
        *.zip) unzip -qo "official/$file" -d official ;;
    esac
done
