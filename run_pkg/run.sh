#!/usr/bin/env bash
DIR="$(cd "$(dirname "$0")" && pwd)"

export LD_LIBRARY_PATH="$DIR/lib:${LD_LIBRARY_PATH}"
export OSG_LIBRARY_PATH="$DIR/osgPlugins"
export GDAL_DATA="$DIR/share/gdal"
export PROJ_LIB="$DIR/share/proj"
export PROJ_DATA="$DIR/share/proj"

exec "$DIR/osgb_converter_1_1" "$@"
