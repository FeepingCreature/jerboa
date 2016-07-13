#!/bin/bash
set -e
prefixes=()
newargs=()
function usage {
  echo "Usage: swigify.sh [-p|--prefix <prefix>]* [header file] [library file] [jb file]"
}
while [ $# -gt 0 ]
do
  case "$1" in
    -p|--prefix)
      prefixes+=("$2")
      shift
      ;;
    -h)
      usage
      exit 1
      ;;
    *)
      newargs+=("$1")
      ;;
  esac
  shift
done

set -- ${newargs[@]}

if [[ $# -lt 3 ]]
then
  usage
  exit 1
fi
HEADER_FILE=$1; shift
LIB_FILE=$1; shift
JB_FILE=$1; shift
echo "-- Running SWIG -> XML"
swig -xml -o swig.xml -module swig -includeall -ignoremissing $@ "$HEADER_FILE"
echo "-- Reencoding as UTF-8"
iconv -f ISO-8859-15 -t UTF-8 swig.xml > swig.2.xml
echo "-- Running XML -> C"
./swig_xml_to_c.jb swig.2.xml "$LIB_FILE" ${prefixes[@]}
echo "-- Building C"
gcc swig_c_gen.c -o swig_c_gen $@
echo "-- Running C -> Jb"
./swig_c_gen > "$JB_FILE"
echo "-- Done."
rm swig.xml swig_c_gen.c swig_c_gen
