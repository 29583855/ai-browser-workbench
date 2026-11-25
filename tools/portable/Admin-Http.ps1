# 用途：允许端点文件原子替换，避免轮询旧端点时阻塞后台重启；日期：2026-11-05.
function Read-AwbAdminEndpoint([string]$Path) {
    $awbShare = [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete
    $awbFile = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, $awbShare)
    try {
        $awbReader = [System.IO.StreamReader]::new($awbFile, [System.Text.Encoding]::UTF8)
        try { return ($awbReader.ReadToEnd() | ConvertFrom-Json) } finally { $awbReader.Dispose() }
    } finally { $awbFile.Dispose() }
}

# 用途：用明确长度的UTF8正文调用后台，兼容Windows PowerShell 5.1且不发送Expect/分块传输；日期：2026-11-05.
function Invoke-AwbAdminHttp([string]$Uri, [string]$Body = '{}', [hashtable]$Headers = @{},
                            [string]$ContentType = 'application/json', [string]$Method = 'Post') {
    $awbRequest = [System.Net.HttpWebRequest]::Create($Uri)
    $awbRequest.Method = $Method
    $awbRequest.ContentType = $ContentType
    $awbRequest.AllowAutoRedirect = $false
    $awbRequest.ServicePoint.Expect100Continue = $false
    $awbRequest.SendChunked = $false
    $awbRequest.Timeout = 15000
    $awbRequest.ReadWriteTimeout = 15000
    foreach ($awbName in $Headers.Keys) { $awbRequest.Headers[$awbName] = $Headers[$awbName] }
    $awbBytes = [System.Text.Encoding]::UTF8.GetBytes($Body)
    $awbRequest.ContentLength = $awbBytes.Length
    $awbStream = $awbRequest.GetRequestStream()
    try { $awbStream.Write($awbBytes, 0, $awbBytes.Length) } finally { $awbStream.Dispose() }
    $awbResponse = $null
    try {
        try { $awbResponse = $awbRequest.GetResponse() }
        catch [System.Net.WebException] {
            if (-not $_.Exception.Response) { throw }
            $awbResponse = $_.Exception.Response
        }
        $awbReader = [System.IO.StreamReader]::new($awbResponse.GetResponseStream(), [System.Text.Encoding]::UTF8)
        try { $awbResult = $awbReader.ReadToEnd() | ConvertFrom-Json } finally { $awbReader.Dispose() }
        if (-not $awbResult.ok) { throw ('Backend rejected request: ' + $awbResult.error.code + ': ' + $awbResult.error.message) }
        return $awbResult
    } finally { if ($awbResponse) { $awbResponse.Dispose() } }
}
