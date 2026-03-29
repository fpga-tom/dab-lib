if (!exists("file")) file = "/tmp/prs_fft.gnuplot.dat"
if (!exists("frame")) frame = 0
if (!exists("pause_s")) pause_s = 0.05
if (!exists("datasets_per_frame")) datasets_per_frame = 151
if (!exists("kind")) kind = "data"
if (!exists("start_symbol")) start_symbol = 0
if (!exists("stop_symbol")) stop_symbol = 74

set term qt
set grid
unset key
set size square
set xlabel "Re"
set ylabel "Im"

do for [sym=start_symbol:stop_symbol] {
    dataset = datasets_per_frame * frame + 1 + (2 * sym)
    color = "#bb3e03"
    title_kind = "DATA"
    if (kind eq "diff") {
        dataset = dataset + 1
        color = "#1d3557"
        title_kind = "DIFF"
    }
    stats file index dataset using 2 nooutput
    x_min = STATS_min
    x_max = STATS_max
    stats file index dataset using 3 nooutput
    y_min = STATS_min
    y_max = STATS_max
    span = x_max - x_min
    span = ((y_max - y_min) > span) ? (y_max - y_min) : span
    span = (span <= 0) ? 1 : span
    pad = 0.1 * span
    cx = 0.5 * (x_min + x_max)
    cy = 0.5 * (y_min + y_max)

    set xrange [cx - 0.5 * span - pad : cx + 0.5 * span + pad]
    set yrange [cy - 0.5 * span - pad : cy + 0.5 * span + pad]
    set title sprintf("Frame %d %s%d", frame, title_kind, sym)
    plot [-2:2][-2:2] file index dataset using 2:3 with points pt 7 ps 0.4 lc rgb color
    pause pause_s
}
