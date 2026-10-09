#!/bin/bash
set -e
while getopts c:t:p:m:v:  opt
do
	case $opt in
		t)
			type=$OPTARG
			;;
		v)
			version=$OPTARG
			;;
		p)
			platform=$OPTARG
			;;
		m)
			model=$OPTARG
			;;
		?)
			echo "unkonwn"
			exit
			;;
       esac
done

help_string=".sh [-t build|push] [-p (amd64|arm64|...,default is `arch`) ] [-m Debug|Release] [-v [version]]"

if [[ ! -n $type ]];then
        echo $help_string
        exit
fi

if [[ ! -n $model ]];then
        echo $help_string
        exit
fi

if [[ ! -n $version ]];then
        echo "use latest no version set"
        version="latest"
fi

if [[ ! -n $platform ]];then
	platform=`arch`
	echo "auto select arch:${platform}" 
fi

case $platform in
"arm64")
	#eg:osx
	platform="linux/arm64"
	;;
"x86_64"|"amd64")
	platform="linux/amd64"
	;;
*)
	echo "unknown cpu-arch ${platform}"
	echo "Use 'docker buildx ls' to get supported ARCH"
	exit
	;;
esac

case $model in
	'Debug')
		;;
	'Release')
		;;
	*)
        echo "unkonwn model"
	echo $help_string
        exit
        ;;
esac

namespace="3spro"
packagename="3spro-mserver"

case $type in
	'build')
	# FFmpeg and the AWS SDK are built from submodules inside the image (COPY . takes the checkout)
	for f in 3rdpart/FFmpeg/configure 3rdpart/nv-codec-headers/Makefile \
		3rdpart/aws-sdk-cpp/crt/aws-crt-cpp/crt/aws-c-common/CMakeLists.txt; do
		if [[ ! -f $f ]]; then
			echo "missing $f, run: git submodule update --init --recursive --depth 1"
			exit 1
		fi
	done
	rm -rf ./build/CMakeCache.txt
	# Take Tencent Cloud Account as an example
	docker buildx build --platform=$platform --network=host --build-arg MODEL=$model -t $namespace/$packagename:$model.$version .
	#docker build --network=host --build-arg MODEL=$model -t $namespace/$packagename:$model.$version .
		;;
	'push')
		echo "push to dst registry"
		# Take Tencent Cloud Account as an example
		docker login --username=s3mediakit
		docker push $namespace/$packagename:$model.$version
		;;
 	*)
		echo "unkonwn type"
		echo $help_string
		exit
		;;
esac
