# Mẫu tải GPU của Windows (WSL2 không có intel_gpu_top/nvidia-smi cho GPU-PV).
# Mỗi dòng: "<epoch> <engtype>=<% tổng mọi tiến trình> ...", ví dụ "1792810812 3d=0.7 videodecode=31.5".
# hwbench.py chạy script này suốt phiên đo rồi lấy trung bình theo cửa sổ thời gian.
param([int]$n = 100000, [int]$iv = 1)
for ($i = 0; $i -lt $n; $i++) {
    $s = (Get-Counter '\GPU Engine(*)\Utilization Percentage' -SampleInterval $iv -MaxSamples 1).CounterSamples
    $g = @{}
    foreach ($c in $s) {
        if ($c.InstanceName -match 'engtype_(\w+)') { $k = $matches[1]; $g[$k] = [double]$g[$k] + $c.CookedValue }
    }
    $line = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds().ToString()
    foreach ($k in ($g.Keys | Sort-Object)) { $line += " " + $k + "=" + [math]::Round($g[$k], 1) }
    Write-Output $line
    [Console]::Out.Flush()
}
