#!/bin/bash
# Check Ni xcoder basic functions

# Global variables
major_version="0"
minor_version="0"
patch_version="0"
AVone_check=""
HWaccel=""
Quadra_suffix=""
FFm311_check=""
libx_bins_dir=""
vids_dir=""
Checksum_check=""

# generate a YUV file of 1280x720p_Basketball.264 if needed
function gen_yuv_file_if_needed() {
    if [ ! -f "${vids_dir}/1280x720p_Basketball.yuv" ]; then
        ./ffmpeg -nostdin -vsync 0 -y -i ${vids_dir}/1280x720p_Basketball.264 -c:v rawvideo ${vids_dir}/1280x720p_Basketball.yuv &> /dev/null
        if [[ $? != 0 ]]; then
            echo -e "\e[31mFAIL\e[0m: cannot generate ${vids_dir}/1280x720p_Basketball.yuv from ${vids_dir}/1280x720p_Basketball.264"
        fi
    fi
}

# check a file vs expected hash, print result
# $1 - file
# $2 - hash
function check_hash() {
    HASH=`md5sum ${1}`
    if [[ ${HASH%% *} == $2 ]]; then
        echo -e "\e[32mPASS: ${1} matches checksum.\e[0m"
    else
        echo -e "\e[31mFAIL: ${1} does not match checksum.\e[0m"
        echo -e "\e[31m      expected: ${2}\e[0m"
        echo -e "\e[31m      ${1}: ${HASH%% *}\e[0m"
    fi
}

# check ffmpeg cmd. First check return code, if pass
#   then check the md5 hash value
# $1 - rc
# $2 - output_file
# $3 - expected hash value
function check_ffmpeg_cmd() {
    if [[ $1 != 0 ]]; then
        echo -e "\e[31mFAIL: return code is ${1}\e[0m"
    else
        echo -e "\e[32mComplete! ${2} has been generated.\e[0m"
        check_hash ${2} ${3}
    fi
}

# Check for passwordless sudo access
# return 0 if true, 124 if false
function sudo_check() {
    timeout -k 1 1 sudo whoami &> /dev/null
    return $?
}

# Function to extract and check ffmpeg version
get_ffmpeg_version() {
    ffmpeg_output=$(./ffmpeg -version 2> /dev/null)

    # Use regex to extract the version number
    if [[ $ffmpeg_output =~ ffmpeg\ version\ ([0-9]+)\.([0-9]+)\.?([0-9]*) ]]; then
        major_version=${BASH_REMATCH[1]}
        minor_version=${BASH_REMATCH[2]}
        patch_version=${BASH_REMATCH[3]:-0}
    fi
}

# Cleanup old files
declare -a Outputs=("output_5.yuv" "output_6.yuv" "output_7.yuv" "output_8.h264" "output_9.h265" "output_10.ivf" "output_11.h265" "output_12.ivf" "output_13.h264"
                    "output_14.ivf" "output_15.h264" "output_16.h265" "output_17.ivf" "output_18.h264" "output_18.h265" "output_18.ivf" "output_19_720p.h265"
                    "output_19_540p.h265" "output_19_360p.h265" "output_20.h264")
for i in "${Outputs[@]}"
do
    sudo_check
    if [[ $? == 0 ]]; then
        sudo rm -f $i
    else
        rm -f $i
    fi
done

# ------------------------------------ MAIN ------------------------------------
# Configure global variables
get_ffmpeg_version
if [ $major_version -ge 5 ]; then
    Checksum_check=1
fi
if [ "$major_version" -eq 3 ] && [ "$minor_version" -eq 1 ] && [ "$patch_version" -eq 1 ]; then
    HWaccel="-hwaccel ni_quadra "
    Quadra_suffix="_FFmpeg3.1.1only"
    FFm311_check=1
fi
if [ "$major_version" -eq 3 ] && [ "$minor_version" -eq 4 ] && [ "$patch_version" -eq 2 ]; then
    AVone_check=1
fi
if [[ -d "../libxcoder$Quadra_suffix/test/" && -d "../libxcoder$Quadra_suffix/build/" ]]; then
    libx_bins_dir="../libxcoder$Quadra_suffix/build"
    vids_dir="../libxcoder$Quadra_suffix/test"
else
    libx_bins_dir="" # use binaries from system $PATH
    vids_dir="./ni_test_vids"
fi

while true;do
options=("check pci device" "check nvme list" "rsrc_init" "ni_rsrc_mon" "test 264 decoder" "test 265 decoder" "test VP9 decoder" "test 264 encoder" "test 265 encoder" 
         "test AV1 encoder" "test 264->265 transcoder" "test 264->AV1 transcoder" "test 265->264 transcoder" "test 265->AV1 transcoder" "test VP9->264 transcoder" 
         "test VP9->265 transcoder" "test VP9->AV1 transcoder" "test split filter" "test scaling filter" "test overlay filter" "Quit")
echo -e "\e[33mChoose an option:\e[0m"
select opt in "${options[@]}"
do
    case $opt in
        "check pci device")
            echo -e "\e[33mYou chose $REPLY which is $opt\e[0m"
            sudo lspci -d 1d82:
            echo
            break
        ;;
        "check nvme list")
            echo -e "\e[33mYou chose $REPLY which is $opt\e[0m"
            sudo nvme list
            echo
            break
        ;;
        "rsrc_init")
            echo -e "\e[33mYou chose $REPLY which is $opt\e[0m"
            if [ -z "$libx_bins_dir" ]; then
                init_rsrc${Quadra_suffix}
            else
                ${libx_bins_dir}/init_rsrc${Quadra_suffix}
            fi
            echo
            break
        ;;
        "ni_rsrc_mon")
            echo -e "\e[33mYou chose $REPLY which is $opt\e[0m"
            if [ -z "$libx_bins_dir" ]; then
                ni_rsrc_mon${Quadra_suffix}
            else
                ${libx_bins_dir}/ni_rsrc_mon${Quadra_suffix}
            fi
            echo
            break
        ;;
        "test 264 decoder")
            echo -e "\e[33mYou chose $REPLY which is $opt\e[0m"
            ##### Decoding H.264 to YUV with NI XCoder (full log) #####
            output_file="output_5.yuv"
            CHECKSUM="be2e62fc528c61a01ac44eae5518e13a"
            cmd="./ffmpeg -y -hide_banner -nostdin -vsync 0 -c:v h264_ni_quadra_dec -i ${vids_dir}/1280x720p_Basketball.264 -c:v rawvideo ${output_file}"
            echo $cmd
            $cmd 2>&1 | tee ${output_file}.log
            check_ffmpeg_cmd ${PIPESTATUS[0]} "${output_file}" "${CHECKSUM}"
            echo
            break
        ;;
        "test 265 decoder")
            echo -e "\e[33mYou chose $REPLY which is $opt\e[0m"
            ##### Decoding H.265 to YUV with NI XCoder (full log) #####
            output_file="output_6.yuv"
            CHECKSUM="f5a29fd3fd2581844848519bafd7370d"
            cmd="./ffmpeg -y -hide_banner -nostdin -vsync 0 -c:v h265_ni_quadra_dec -i ${vids_dir}/akiyo_352x288p25.265 -c:v rawvideo ${output_file}"
            echo $cmd
            $cmd 2>&1 | tee ${output_file}.log
            check_ffmpeg_cmd ${PIPESTATUS[0]} "${output_file}" "${CHECKSUM}"
            echo
            break
        ;;
        "test VP9 decoder")
            if [[ $FFm311_check ]]; then
                echo -e "\e[31m VP9 cannot be run on 3.1.1, stopping test.\e[0m"
                break
            fi
            echo -e "\e[33mYou chose $REPLY which is $opt\e[0m"
            ##### Decoding VP9 to YUV with NI XCoder (full log) #####
            output_file="output_7.yuv"
            CHECKSUM="0da8a892f4f835cd8d8f0c02f208e1f6"
            cmd="./ffmpeg -y -hide_banner -nostdin -vsync 0 -c:v vp9_ni_quadra_dec -i ${vids_dir}/akiyo_352x288p25_300.ivf -c:v rawvideo ${output_file}"
            echo $cmd
            $cmd 2>&1 | tee ${output_file}.log
            check_ffmpeg_cmd ${PIPESTATUS[0]} "${output_file}" "${CHECKSUM}"
            echo
            break
        ;;
        "test 264 encoder")
            echo -e "\e[33mYou chose $REPLY which is $opt\e[0m"
            gen_yuv_file_if_needed
            ##### Encoding YUV to H.264 with NI XCoder (full log) #####
            output_file="output_8.h264"
            CHECKSUM="2b03b92dae709c018b36fa4b8d39d39d"
            cmd="./ffmpeg -y -hide_banner -nostdin -f rawvideo -pix_fmt yuv420p -s:v 1280x720 -r 25 -i ${vids_dir}/1280x720p_Basketball.yuv -c:v h264_ni_quadra_enc ${output_file}"
            echo $cmd
            $cmd 2>&1 | tee ${output_file}.log
            check_ffmpeg_cmd ${PIPESTATUS[0]} "${output_file}" "${CHECKSUM}"
            echo
            break
        ;;
        "test 265 encoder")
            echo -e "\e[33mYou chose $REPLY which is $opt\e[0m"
            gen_yuv_file_if_needed
            ##### Encoding YUV to H.265 with NI XCoder (full log) #####
            output_file="output_9.h265"
            CHECKSUM="0b9b5ec94985b4019eadf177c64859fd"
            cmd="./ffmpeg -y -hide_banner -nostdin -f rawvideo -pix_fmt yuv420p -s:v 1280x720 -r 25 -i ${vids_dir}/1280x720p_Basketball.yuv -c:v h265_ni_quadra_enc ${output_file}"
            echo $cmd
            $cmd 2>&1 | tee ${output_file}.log
            check_ffmpeg_cmd ${PIPESTATUS[0]} "${output_file}" "${CHECKSUM}"
            echo
            break
        ;;
        "test AV1 encoder")
            if [[ $AVone_check || $FFm311_check ]]; then
                echo -e "\e[31m AV1 encoder cannot be run on 3.4.2 or 3.1.1, stopping test.\e[0m"
                break
            fi
            echo -e "\e[33mYou chose $REPLY which is $opt\e[0m"
            gen_yuv_file_if_needed
            ##### Encoding YUV to AV1 with NI XCoder (full log) #####
            output_file="output_10.ivf"
            CHECKSUM="3395048a62ead3a02939cbc02e6d1df9"
            cmd="./ffmpeg -y -hide_banner -nostdin -f rawvideo -pix_fmt yuv420p -s:v 1280x720 -r 25 -i ${vids_dir}/1280x720p_Basketball.yuv -c:v av1_ni_quadra_enc ${output_file}"
            echo $cmd
            $cmd 2>&1 | tee ${output_file}.log
            check_ffmpeg_cmd ${PIPESTATUS[0]} "${output_file}" "${CHECKSUM}"
            echo
            break
        ;;
        "test 264->265 transcoder")
            echo -e "\e[33mYou chose $REPLY which is $opt\e[0m"
            ##### Transcoding H.264 to H.265 with NI XCoder (full log) #####
            output_file="output_11.h265"
            CHECKSUM="ed5b81ed16ff61349f40ae66e54d67b9"
            cmd="./ffmpeg -y -hide_banner -nostdin -vsync 0 -c:v h264_ni_quadra_dec -i ${vids_dir}/1280x720p_Basketball.264 -c:v h265_ni_quadra_enc ${output_file}"
            echo $cmd
            $cmd 2>&1 | tee ${output_file}.log
            check_ffmpeg_cmd ${PIPESTATUS[0]} "${output_file}" "${CHECKSUM}"
            echo
            break
        ;;
        "test 264->AV1 transcoder")
            if [[ $AVone_check || $FFm311_check ]]; then
                echo -e "\e[31m AV1 cannot be run on 3.4.2 or 3.1.1, stopping test.\e[0m"
                break
            fi
            echo -e "\e[33mYou chose $REPLY which is $opt\e[0m"
            ##### Transcoding H.264 to AV1 with NI XCoder (full log) #####
            output_file="output_12.ivf"
            CHECKSUM="ac7151a66be39c568ac4dea3f14730a3"
            cmd="./ffmpeg -y -hide_banner -nostdin -vsync 0 -c:v h264_ni_quadra_dec -i ${vids_dir}/1280x720p_Basketball.264 -c:v av1_ni_quadra_enc ${output_file}"
            echo $cmd
            $cmd 2>&1 | tee ${output_file}.log
            check_ffmpeg_cmd ${PIPESTATUS[0]} "${output_file}" "${CHECKSUM}"
            echo
            break
        ;;
        "test 265->264 transcoder")
            echo -e "\e[33mYou chose $REPLY which is $opt\e[0m"
            ##### Transcoding H.265 to H.264 with NI XCoder (full log) #####
            output_file="output_13.h264"
            CHECKSUM="06385cc65ffe0a8697046443213a3c51"
            cmd="./ffmpeg -y -hide_banner -nostdin -vsync 0 -c:v h265_ni_quadra_dec -i ${vids_dir}/akiyo_352x288p25.265 -c:v h264_ni_quadra_enc ${output_file}"
            echo $cmd
            $cmd 2>&1 | tee ${output_file}.log
            check_ffmpeg_cmd ${PIPESTATUS[0]} "${output_file}" "${CHECKSUM}"
            echo
            break
        ;;
        "test 265->AV1 transcoder")
            if [[ $AVone_check || $FFm311_check ]]; then
                echo -e "\e[31m AV1 cannot be run on 3.4.2 or 3.1.1, stopping test.\e[0m"
                break
            fi
            echo -e "\e[33mYou chose $REPLY which is $opt\e[0m"
            ##### Transcoding H.265 to AV1 with NI XCoder (full log) #####
            output_file="output_14.ivf"
            CHECKSUM="cc5b1227acd0e0c6485ed65859de1e67"
            cmd="./ffmpeg -y -hide_banner -nostdin -vsync 0 -c:v h265_ni_quadra_dec -i ${vids_dir}/akiyo_352x288p25.265 -c:v av1_ni_quadra_enc ${output_file}"
            echo $cmd
            $cmd 2>&1 | tee ${output_file}.log
            check_ffmpeg_cmd ${PIPESTATUS[0]} "${output_file}" "${CHECKSUM}"
            echo
            break
        ;;
        "test VP9->264 transcoder")
            if [[ $FFm311_check ]]; then
                echo -e "\e[31m VP9 cannot be run on 3.1.1, stopping test.\e[0m"
                break
            fi
            echo -e "\e[33mYou chose $REPLY which is $opt\e[0m"
            ##### Transcoding VP9 to H.264 with NI XCoder (full log) #####
            output_file="output_15.h264"
            CHECKSUM="1c683ddf68c3fd3b733ccf105a2d2cec"
            cmd="./ffmpeg -y -hide_banner -nostdin -vsync 0 -c:v vp9_ni_quadra_dec -i ${vids_dir}/akiyo_352x288p25_300.ivf -c:v h264_ni_quadra_enc ${output_file}"
            echo $cmd
            $cmd 2>&1 | tee ${output_file}.log
            check_ffmpeg_cmd ${PIPESTATUS[0]} "${output_file}" "${CHECKSUM}"
            echo
            break
        ;;
        "test VP9->265 transcoder")
            if [[ $FFm311_check ]]; then
                echo -e "\e[31m VP9 cannot be run on 3.1.1, stopping test.\e[0m"
                break
            fi
            echo -e "\e[33mYou chose $REPLY which is $opt\e[0m"
            ##### Transcoding VP9 to H.265 with NI XCoder (full log) #####
            output_file="output_16.h265"
            CHECKSUM="37fcf33399c5b4355d30d9faa1c335eb"
            cmd="./ffmpeg -y -hide_banner -nostdin -vsync 0 -c:v vp9_ni_quadra_dec -i ${vids_dir}/akiyo_352x288p25_300.ivf -c:v h265_ni_quadra_enc ${output_file}"
            echo $cmd
            $cmd 2>&1 | tee ${output_file}.log
            check_ffmpeg_cmd ${PIPESTATUS[0]} "${output_file}" "${CHECKSUM}"
            echo
            break
        ;;
        "test VP9->AV1 transcoder")
            if [[ $AVone_check || $FFm311_check ]]; then
                echo -e "\e[31m AV1 cannot be run on 3.4.2 or 3.1.1, stopping test.\e[0m"
                break
            fi
            echo -e "\e[33mYou chose $REPLY which is $opt\e[0m"
            ##### Transcoding VP9 to H.265 with NI XCoder (full log) #####
            output_file="output_17.ivf"
            CHECKSUM="103282f28105fa0979d358f590035d20"
            cmd="./ffmpeg -y -hide_banner -nostdin -vsync 0 -c:v vp9_ni_quadra_dec -i ${vids_dir}/akiyo_352x288p25_300.ivf -c:v av1_ni_quadra_enc ${output_file}"
            echo $cmd
            $cmd 2>&1 | tee ${output_file}.log
            check_ffmpeg_cmd ${PIPESTATUS[0]} "${output_file}" "${CHECKSUM}"
            echo
            break
        ;;
        "test split filter")
            if [[ $AVone_check || $FFm311_check ]]; then
                echo -e "\e[31m AV1 cannot be run on 3.4.2 or 3.1.1, stopping test.\e[0m"
                break
            fi
            echo -e "\e[33mYou chose $REPLY which is $opt\e[0m"
            ##### Split filter H.264 to H.264, H.265 and AV1 with NI XCoder (full log) #####
            output_file1="output_18.h264"
            output_file2="output_18.h265"
            output_file3="output_18.ivf"
            cmd=(./ffmpeg ${HWaccel}-y -hide_banner -nostdin -vsync 0 -c:v h264_ni_quadra_dec -xcoder-params out=hw -dec 0 -i ${vids_dir}/1280x720p_Basketball.264 -filter_complex "[0:v]ni_quadra_split=3:0:0[out1][out2][out3]" -map [out1] -c:v h264_ni_quadra_enc -enc 0 ${output_file1} -map [out2] -c:v h265_ni_quadra_enc -enc 0 ${output_file2} -map [out3] -c:v av1_ni_quadra_enc -enc 0 ${output_file3})
            echo "${cmd[@]}"
            "${cmd[@]}" 2>&1 | tee output_18.log
            if [[ ${PIPESTATUS[0]} != 0 ]]; then
                    echo -e "\e[31mFAIL Complete! Non-zero return code ${PIPESTATUS[0]}. ffmpeg command failed. \e[0m"
            else
                    echo -e "\e[33mComplete! ${output_file1}, ${output_file2} and ${output_file3} have been generated.\e[0m"
                    CHECKSUM1="ca099783624e628858ddb711972fb31c"
                    CHECKSUM2="ed5b81ed16ff61349f40ae66e54d67b9"
                    CHECKSUM3="ac7151a66be39c568ac4dea3f14730a3"
                    check_hash "${output_file1}" "${CHECKSUM1}"
                    check_hash "${output_file2}" "${CHECKSUM2}"
                    check_hash "${output_file3}" "${CHECKSUM3}"
            fi
            echo
            break
        ;;
        "test scaling filter")
            echo -e "\e[33mYou chose $REPLY which is $opt\e[0m"
            ##### Scale filter H.264 to H.264 with NI XCoder (full log) #####
            output_file1="output_19_720p.h265"
            output_file2="output_19_540p.h265"
            output_file3="output_19_360p.h265"
            cmd=(./ffmpeg ${HWaccel}-y -hide_banner -nostdin -vsync 0 -xcoder-params out=hw -c:v h264_ni_quadra_dec -dec 0 -i ${vids_dir}/1280x720p_Basketball.264 -filter_complex "[0:v]split=2[out720p][in720p];[in720p]ni_quadra_scale=960:540,split=2[out540p][in540p];[in540p]ni_quadra_scale=640:360[out360p]" -map [out720p] -c:v h265_ni_quadra_enc -enc 0 ${output_file1} -map [out540p] -c:v h265_ni_quadra_enc -enc 0 ${output_file2} -map [out360p] -c:v h265_ni_quadra_enc -enc 0 ${output_file3})
            echo "${cmd[@]}"
            "${cmd[@]}" 2>&1 | tee output_19.log
            if [[ ${PIPESTATUS[0]} != 0 ]]; then
                    echo -e "\e[31mFAIL Complete! Non-zero return code ${PIPESTATUS[0]}. ffmpeg command failed. \e[0m"
            else
                    echo -e "\e[33mComplete! ${output_file1}, ${output_file2} and ${output_file3} have been generated.\e[0m"
                    CHECKSUM1="ed5b81ed16ff61349f40ae66e54d67b9"
                    CHECKSUM2="b55a75669e93e87a30f5193a4a5ddbd5"
                    CHECKSUM3="ce403775a56e740e5e9f13006c5e126a"
                    check_hash "${output_file1}" "${CHECKSUM1}"
                    check_hash "${output_file2}" "${CHECKSUM2}"
                    check_hash "${output_file3}" "${CHECKSUM3}"
            fi
            echo
            break
        ;;
        "test overlay filter")
            if [[ $FFm311_check ]]; then
                echo -e "\e[31m VP9 cannot be run on 3.1.1, stopping test.\e[0m"
                break
            fi
            echo -e "\e[33mYou chose $REPLY which is $opt\e[0m"
            ##### Overlay filter H.264 and VP9 to H.264 with NI XCoder (full log) #####
            output_file="output_20.h264"
            CHECKSUM="ce1d94d022812d1f68a11a0b0b66a5a2"
            if [[ $Checksum_check ]]; then
                    CHECKSUM="441455d4cc8d14e35453360a69c760d5"
            fi
            cmd=(./ffmpeg -y -hide_banner -nostdin -vsync 0 -c:v h264_ni_quadra_dec ${HWaccel}-xcoder-params out=hw -dec 0 -i ${vids_dir}/1280x720p_Basketball.264 -c:v vp9_ni_quadra_dec ${HWaccel}-xcoder-params out=hw -dec 0 -i ${vids_dir}/akiyo_352x288p25_300.ivf -filter_complex "[0:v][1:v]ni_quadra_overlay=0:0[out]" -map [out] -c:v h264_ni_quadra_enc -enc 0 ${output_file})
            echo "${cmd[@]}"
            "${cmd[@]}" 2>&1 | tee ${output_file}.log
            check_ffmpeg_cmd ${PIPESTATUS[0]} "${output_file}" "${CHECKSUM}"
            echo
            break
        ;;
        "Quit")
            break 2
        ;;
        *)
            echo -e "\e[31mInvalid choice!\e[0m"
            echo
            break
        ;;
    esac
done
done
echo -e "\e[33mBye!\e[0m"
echo
