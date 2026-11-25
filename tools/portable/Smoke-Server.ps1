param([Parameter(Mandatory = $true)][string]$StateDirectory)
$ErrorActionPreference = 'Stop'
$awbListener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
$awbListener.Start()
$awbPort = $awbListener.LocalEndpoint.Port
$awbEncoding = [System.Text.UTF8Encoding]::new($false)
[System.IO.File]::WriteAllText((Join-Path $StateDirectory 'endpoint.txt'), "http://127.0.0.1:$awbPort", $awbEncoding)
$awbHtml = @'
<!doctype html><html lang="en"><meta charset="utf-8"><title>Portable offline smoke</title>
<body><label>Name <input id="name"></label><button id="submit">Submit</button><p id="result">ready</p>
<script>
// 用途：发送离线自检动作并显示服务器确认；日期：2026-11-04.
document.querySelector('#submit').onclick = async () => {
 const value = document.querySelector('#name').value;
 const response = await fetch('/submit?value=' + encodeURIComponent(value));
 document.querySelector('#result').textContent = await response.text();
};
</script></body></html>
'@
try {
    while ($true) {
        $awbClient = $awbListener.AcceptTcpClient()
        $awbClient.ReceiveTimeout = 5000
        $awbClient.SendTimeout = 5000
        try {
            $awbStream = $awbClient.GetStream()
            $awbReader = [System.IO.StreamReader]::new($awbStream, [System.Text.Encoding]::ASCII, $false, 4096, $true)
            $awbRequest = $awbReader.ReadLine()
            $awbHeaderBytes = 0
            do {
                $awbHeader = $awbReader.ReadLine()
                $awbHeaderBytes += $awbHeader.Length
                if ($awbHeaderBytes -gt 16384) { throw 'Fixture header limit exceeded.' }
            } while ($awbHeader)
            $awbUrl = if ($awbRequest) { ($awbRequest -split ' ')[1] } else { '/' }
            $awbStatus = '200 OK'
            $awbContentType = 'text/html; charset=utf-8'
            $awbBody = $awbHtml
            if ($awbUrl -eq '/submit?value=portable-smoke') {
                $awbBody = 'server-confirmed:portable-smoke'
                $awbContentType = 'text/plain; charset=utf-8'
                [System.IO.File]::WriteAllText((Join-Path $StateDirectory 'receipt.txt'), '1', $awbEncoding)
            } elseif ($awbUrl -ne '/') {
                $awbStatus = '404 Not Found'
                $awbBody = 'not found'
            }
            $awbBodyBytes = $awbEncoding.GetBytes($awbBody)
            $awbHeaders = "HTTP/1.1 $awbStatus`r`nContent-Type: $awbContentType`r`nContent-Length: $($awbBodyBytes.Length)`r`nConnection: close`r`nCache-Control: no-store`r`n`r`n"
            $awbHeaderData = [System.Text.Encoding]::ASCII.GetBytes($awbHeaders)
            $awbStream.Write($awbHeaderData, 0, $awbHeaderData.Length)
            $awbStream.Write($awbBodyBytes, 0, $awbBodyBytes.Length)
            $awbStream.Flush()
            $awbReader.Dispose()
        } catch [System.IO.IOException] {
            # 用途：浏览器预连接断开或超时只结束该连接，不中断自检服务器；日期：2026-11-04.
        } finally { $awbClient.Dispose() }
    }
} finally { $awbListener.Stop() }
