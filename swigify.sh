#!/bin/bash
set -e
swigargs=()
newargs=()
function usage {
  echo "Usage: swigify.sh [-p|--prefix <prefix>]* [header file] [library file] [jb file]"
}
while [ $# -gt 0 ]
do
  case "$1" in
    -p|--prefix)
      swigargs+=("$2")
      shift
      ;;
    -x|--exclude)
      swigargs+=("-x")
      swigargs+=("$2")
      shift
      ;;
    -d|--define)
      swigargs+=("-d")
      swigargs+=("$2")
      swigargs+=("$3")
      shift
      shift
      ;;
    -i|--include)
      swigargs+=("-i")
      swigargs+=("$2")
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

swigcmdargs=()
newargs=()
while [ $# -gt 0 ]
do
  case "$1" in
    -cpperraswarn)
      swigcmdargs+=("$1")
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
swig -xml -o swig.xml -module swig -includeall -ignoremissing ${swigcmdargs[@]} $@ "$HEADER_FILE"
echo "-- Reencoding as UTF-8"
iconv -f ISO-8859-15 -t UTF-8 swig.xml > swig.2.xml
echo "-- Running XML -> C"
./swig_xml_to_c.jb swig.2.xml "$LIB_FILE" ${swigargs[@]}
echo "-- Building C"
gcc $CFLAGS swig_c_gen.c -o swig_c_gen $@
echo "-- Running C -> Jb"
./swig_c_gen > "$JB_FILE"
echo "-- Done."
rm swig.xml swig.2.xml swig_c_gen.c swig_c_gen
