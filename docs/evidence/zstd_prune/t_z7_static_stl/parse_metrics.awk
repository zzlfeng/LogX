/^==== Stage A/ { stage="A"; mode=""; next }
/^==== Stage B/ { stage="B"; mode=""; next }
/^==== Stage C/ { stage="C"; mode=""; next }
/^==== Stage D/ { stage="D"; mode=""; next }
/^-- compress=none  thr=0/ { mode="pure_none" }
/^-- compress=zstd  thr=8KB/ { mode="prod_zstd" }
/^-- compress=none --/ { mode="B_none" }
/^-- compress=zstd --/ { mode="B_zstd" }
stage=="A" && mode!="" && /^[ \t]+P50[ \t]+=/ { p50[mode]=$3 }
stage=="A" && mode!="" && /^[ \t]+P99[ \t]+=/ { p99[mode]=$3 }
stage=="B" && /^[ \t]+纯写吞吐:/ { if (mode=="B_none") thr_none=$2; else if (mode=="B_zstd") thr_zstd=$2 }
stage=="B" && /payload\/落盘压缩比 = / { ratio=$NF }
stage=="C" && /^[ \t]+P99[ \t]+=/ { flush=$3 }
END {
    printf "pure_none_P50=%s pure_none_P99=%s prod_zstd_P50=%s prod_zstd_P99=%s B_none_thr=%s B_zstd_thr=%s ratio=%s flush_P99=%s\n",
        p50["pure_none"], p99["pure_none"], p50["prod_zstd"], p99["prod_zstd"], thr_none, thr_zstd, ratio, flush
}
