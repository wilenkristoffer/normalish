# Fetches the monocular depth model normalish uses for the live webcam feed.
#
# Not committed to the repository: it is about 99 MB, which does not belong in
# git whatever its licence. Depth mode stays unavailable, with a reason shown
# in the panel, until this has been run once.
#
# Depth-Anything-V2-Metric-Hypersim-Small, Apache-2.0. The *metric* variant
# matters: it reports depth in metres, which is the space normalish already
# lights in, rather than the unitless inverse depth the base models produce.

$ErrorActionPreference = 'Stop'

$dir = 'C:\ProgramData\normalish'
$dest = Join-Path $dir 'depth-anything-v2-metric-small.onnx'
$url = 'https://huggingface.co/77ukhtar/depth-anything-v2-metric-onnx/resolve/main/model.onnx'
$expectedBytes = 98941181

if (-not (Test-Path $dir)) {
    New-Item -ItemType Directory -Path $dir | Out-Null
    Write-Output "created $dir"
}

if (Test-Path $dest) {
    $have = (Get-Item $dest).Length
    if ($have -eq $expectedBytes) {
        Write-Output "already present: $dest"
        exit 0
    }
    Write-Output "size mismatch ($have bytes, expected $expectedBytes) - refetching"
}

Write-Output "downloading ~94 MiB from huggingface.co ..."
# Invoke-WebRequest's progress bar makes a large download several times
# slower in some hosts, so it is turned off for the duration.
$previous = $ProgressPreference
$ProgressPreference = 'SilentlyContinue'
try {
    Invoke-WebRequest -Uri $url -OutFile $dest -UseBasicParsing
} finally {
    $ProgressPreference = $previous
}

$got = (Get-Item $dest).Length
if ($got -ne $expectedBytes) {
    Remove-Item $dest
    throw "downloaded $got bytes, expected $expectedBytes - removed the partial file"
}

Write-Output "done: $dest"
Write-Output "normalish will pick it up on the next start."
