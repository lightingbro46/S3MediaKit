# k8s deployment suggestions

## Compilation

- Method 1

    You can write scripts and compile them yourself

- Method 2

    You can use the built-in `build_docker_images.sh` script, see [Deployment] (##Deployment)

## Deployment
- It can be compiled and pushed to the specified repository using the build_docker_images.sh script under the root directory

    - Push

        Before pushing, be sure to modify the `mirror warehouse username and warehouse address` script. You can also modify the namespace and package name at the same time if necessary.

    - Compilation

        ```shell
        sh build_docker_images.sh [-t build|push] [-m Debug|Release] [-v [version]]
        -t: Specify the compilation type, build the compilation image push to push to the specified repository
        -m: Compile type
        -v: Version number
        ```

- If you need to customize the configuration file, you can use `configMap` to mount it into the pod`/opt/media/conf/` directory to override the default configuration file
- If you need a custom certificate, please replace the `default.pem` certificate file under the `tests` directory of the source code directory. zlmedia will be loaded by default when the pod is started.

