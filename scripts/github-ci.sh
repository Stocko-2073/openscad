#!/usr/bin/env bash

set -e

PARALLEL_MAKE=-j2  # runners have insufficient memory for -j4

BUILDDIR=b

do_experimental() {
	echo "do_experimental()"
	EXPERIMENTAL="-DEXPERIMENTAL=ON"
}

do_enable_python() {
	echo "do_enable_python()"
	PYTHON_DEFINE="-DENABLE_PYTHON=ON"
}

do_qt5() {
	echo "do_qt5()"
	QT="-DUSE_QT6=OFF"
}

do_qt6() {
	echo "do_qt6()"
	QT=""
}

do_build() {
	echo "do_build()"

	rm -rf "$BUILDDIR"
	mkdir "$BUILDDIR"
	(
		cd "$BUILDDIR"
		cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_UNITY_BUILD=OFF ${EXPERIMENTAL} ${PYTHON_DEFINE} ${QT} .. && make $PARALLEL_MAKE
	)
	if [[ $? != 0 ]]; then
		echo "Build failure"
		exit 1
	fi
}

for func in $@
do
	"do_$func"
done

exit 0
