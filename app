#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

usage() {
    cat <<'EOF'
Usage:
  ./app generate <app> <android|ios|hongmeng>
  ./app build    <app> <android|ios>
  ./app open     <app> <android|ios|hongmeng>
EOF
}

if [[ $# -ne 3 ]]; then
    usage >&2
    exit 2
fi

ACTION="$1"
APP_NAME="$2"
PLATFORM="$3"
GENERATED_DIR="${ROOT}/build/apps/${APP_NAME}/${PLATFORM}"
DIST_DIR="${ROOT}/dist/${APP_NAME}/${PLATFORM}"

case "${PLATFORM}" in
    android|ios|hongmeng) ;;
    *)
        usage >&2
        exit 2
        ;;
esac

cmake \
    "-DEVK_ROOT=${ROOT}" \
    "-DEVK_APP=${APP_NAME}" \
    "-DEVK_PLATFORM=${PLATFORM}" \
    -P "${ROOT}/cmake/generate_app.cmake"

case "${ACTION}" in
    generate)
        echo "${GENERATED_DIR}"
        ;;
    build)
        case "${PLATFORM}" in
            android)
                if [[ -x /usr/libexec/java_home ]]; then
                    export JAVA_HOME="$(/usr/libexec/java_home -v 17)"
                fi
                chmod +x "${GENERATED_DIR}/gradlew"
                "${GENERATED_DIR}/gradlew" --project-dir "${GENERATED_DIR}" assembleDebug

                ANDROID_APK="${GENERATED_DIR}/app/build/outputs/apk/debug/app-debug.apk"
                ARTIFACT="${DIST_DIR}/${APP_NAME}-debug.apk"
                mkdir -p "${DIST_DIR}"
                cp "${ANDROID_APK}" "${ARTIFACT}"
                ;;
            ios)
                xcodebuild \
                    -project "${GENERATED_DIR}/EVKApp.xcodeproj" \
                    -scheme EVKApp \
                    -configuration Debug \
                    -sdk iphoneos \
                    -destination generic/platform=iOS \
                    -derivedDataPath "${GENERATED_DIR}/DerivedData" \
                    CODE_SIGNING_ALLOWED=NO \
                    build

                IOS_PRODUCTS_DIR="${GENERATED_DIR}/DerivedData/Build/Products/Debug-iphoneos"
                shopt -s nullglob
                IOS_APP_BUNDLES=("${IOS_PRODUCTS_DIR}"/*.app)
                shopt -u nullglob
                if [[ ${#IOS_APP_BUNDLES[@]} -ne 1 ]]; then
                    echo "Expected exactly one .app in ${IOS_PRODUCTS_DIR}" >&2
                    exit 1
                fi

                APP_BUNDLE_NAME="$(basename "${IOS_APP_BUNDLES[0]}")"
                ARTIFACT="${DIST_DIR}/${APP_BUNDLE_NAME}"
                mkdir -p "${DIST_DIR}"
                rm -rf "${ARTIFACT}"
                ditto "${IOS_APP_BUNDLES[0]}" "${ARTIFACT}"
                ;;
            hongmeng)
                echo "HarmonyOS builds require DevEco Studio. Use: ./app open ${APP_NAME} hongmeng" >&2
                exit 2
                ;;
        esac
        echo "App artifact: ${ARTIFACT}"
        ;;
    open)
        case "${PLATFORM}" in
            android)
                if ! open -a "Android Studio" "${GENERATED_DIR}" 2>/dev/null; then
                    echo "Open this project in Android Studio: ${GENERATED_DIR}"
                fi
                ;;
            ios)
                open "${GENERATED_DIR}/EVKApp.xcodeproj"
                ;;
            hongmeng)
                if ! open -a "DevEco Studio" "${GENERATED_DIR}" 2>/dev/null; then
                    echo "Open this project in DevEco Studio: ${GENERATED_DIR}"
                fi
                ;;
        esac
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac
