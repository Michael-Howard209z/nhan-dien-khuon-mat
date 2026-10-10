// ============================================================================
//  TRANG CAU HINH (che do AP) - duoc phuc tai /setup va tai / khi dang o AP
//  Nhe (khong gzip) de ESP32 khong phai nen, trang nay chi mo 1 lan khi lap dat.
// ============================================================================
#pragma once

static const char SETUP_HTML[] = R"SETUPHTML(
<!DOCTYPE html>
<html lang="vi">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Cấu hình ESP32-CAM</title>
<style>
*{box-sizing:border-box}
body{font-family:system-ui,Arial,sans-serif;margin:0;background:#0f172a;color:#e2e8f0}
.wrap{max-width:520px;margin:0 auto;padding:16px}
h1{font-size:20px;margin:4px 0 12px}
h2{font-size:15px;margin:22px 0 8px;color:#7dd3fc;border-bottom:1px solid #1e293b;padding-bottom:6px}
label{display:block;font-size:13px;color:#94a3b8;margin:10px 0 3px}
input[type=text],input[type=password],select{width:100%;padding:11px;font-size:15px;border-radius:8px;border:1px solid #334155;background:#1e293b;color:#f1f5f9}
input:focus,select:focus{outline:2px solid #38bdf8}
.chk{display:flex;align-items:center;gap:8px;font-size:14px;color:#e2e8f0;margin:12px 0}
.chk input{width:20px;height:20px}
button{cursor:pointer;border:0;border-radius:8px;padding:11px 14px;font-size:14px;background:#334155;color:#f1f5f9;margin-top:10px}
button:active{transform:translateY(1px)}
.b-primary{background:#2563eb;font-weight:700;width:100%;padding:14px;font-size:16px}
.b-danger{background:#7f1d1d;color:#fecaca}
.hint{font-size:12px;color:#64748b;line-height:1.5;margin:6px 0}
.banner{padding:10px 12px;border-radius:8px;font-size:13px;background:#1e3a5f;color:#bae6fd;margin-bottom:4px}
#scanlist div{padding:10px;margin:4px 0;background:#1e293b;border-radius:8px;font-size:14px;display:flex;justify-content:space-between}
#scanlist b{cursor:pointer;color:#7dd3fc}
#msg{margin-top:12px;font-size:14px;min-height:20px}
.ok{color:#86efac}.err{color:#fca5a5}
#pv{width:100%;border-radius:8px;margin-top:10px;display:none;background:#000}
.grid2{display:flex;gap:10px}
.grid2>div{flex:1}
</style>
</head>
<body>
<div class="wrap">
<h1>⚙️ Cấu hình ESP32-CAM</h1>
<div id="mode" class="banner">Đang tải...</div>

<h2>1. Mạng WiFi (ESP32 nối vào)</h2>
<label>SSID</label>
<input type="text" id="wifi_ssid" autocomplete="off" placeholder="Ten mang WiFi">
<label>Mật khẩu</label>
<input type="password" id="wifi_pass" placeholder="(để trống nếu mạng mở)">
<button onclick="doScan()">🔍 Quét mạng WiFi</button>
<div id="scanlist"></div>

<h2>2. IP tĩnh (tuỳ chọn)</h2>
<div class="chk"><input type="checkbox" id="use_static"><span>Dùng IP tĩnh (bỏ chọn = tự động DHCP)</span></div>
<div class="grid2">
<div><label>Địa chỉ IP</label><input type="text" id="static_ip" placeholder="192.168.1.100"></div>
<div><label>Gateway</label><input type="text" id="static_gw" placeholder="192.168.1.1"></div>
</div>
<div class="grid2">
<div><label>Subnet mask</label><input type="text" id="static_mask" placeholder="255.255.255.0"></div>
<div><label>DNS</label><input type="text" id="static_dns" placeholder="8.8.8.8"></div>
</div>

<h2>3. Server nhận diện (điểm danh)</h2>
<label>URL endpoint nhận ảnh</label>
<input type="text" id="push_url" placeholder="http://192.168.1.9:5001/api/esp32/frame">
<p class="hint">Nhập IP hoặc tên miền của máy chạy <b>python server.py</b> + cổng CAMERA_PORT (mặc định 5001).
Mỗi lần nhấn nút, ESP32 sẽ POST ảnh tới địa chỉ này.</p>

<h2>4. Hướng camera</h2>
<label>Lật dọc (vflip)</label>
<select id="vflip"><option value="-1">Tự động theo máy ảnh</option><option value="1">Lật (mặc định nếu ảnh bị ngược)</option><option value="0">Không lật</option></select>
<label>Lật ngang (hmirror)</label>
<select id="hmirror"><option value="-1">Tự động theo máy ảnh</option><option value="1">Lật (ảnh bị soi gương)</option><option value="0">Không lật</option></select>
<div class="chk"><input type="checkbox" id="pv_on" onchange="togglePv()"><span>Xem ảnh trực tiếp để chỉnh hướng</span></div>
<img id="pv">

<h2>5. Mạng WiFi tự tạo (khi không nối được WiFi)</h2>
<label>Tên mạng (SSID)</label>
<input type="text" id="ap_ssid" placeholder="ESP32-CAM-Setup">
<label>Mật khẩu (≥ 8 ký tự, để trống = mở)</label>
<input type="password" id="ap_pass">
<p class="hint">Khi mất WiFi, camera tự phát mạng trên. Kết nối vào đó rồi mở
<b>http://192.168.4.1/</b> để vào trang này (điện thoại tự hiện trang captive portal).</p>

<button class="b-primary" onclick="doSave()">💾 Lưu cấu hình</button>
<button class="b-danger" onclick="doReset()">Khôi phục mặc định</button>
<div id="msg"></div>
</div>
<script>
var D;
function $(i){return document.getElementById(i)}
function set(i,v){$(i).value=(v===undefined||v===null)?'':v}
function msg(t,c){var m=$('msg');m.textContent=t;m.className=c||''}

fetch('/api/cfg').then(function(r){return r.json()}).then(function(d){
  D=d;
  set('wifi_ssid',d.wifi.ssid);
  $('use_static').checked=!!d.static.use;
  set('static_ip',d.static.ip);set('static_gw',d.static.gw);
  set('static_mask',d.static.mask);set('static_dns',d.static.dns);
  set('push_url',d.push_url);
  set('vflip',String(d.cam.vflip));set('hmirror',String(d.cam.hmirror));
  set('ap_ssid',d.ap.ssid);set('ap_pass',d.ap.pass);
  $('mode').textContent=(d.setup_mode?'🔴 Đang ở chế độ CẤU HINH (chưa có WiFi)':'🟢 Đã kết nối WiFi')
    +' — IP: '+d.net.ip+(d.setup_mode?' — mạng: '+d.ap.ssid:' — SSID: '+d.wifi.ssid);
}).catch(function(){msg('Không đọc được cấu hình từ thiết bị!','err')});

function doScan(){
  msg('Đang quét...');$('scanlist').innerHTML='';
  fetch('/api/scan').then(function(r){return r.json()}).then(function(d){
    var e=$('scanlist');
    if(!d.list||!d.list.length){msg('Không thấy mạng WiFi nào','err');return}
    msg('');
    d.list.forEach(function(n){
      var r=document.createElement('div');
      r.innerHTML='<span>'+n.ssid+' '+(n.secure?'🔒':'')+'</span><b>chọn</b>';
      r.querySelector('b').onclick=function(){$('wifi_ssid').value=n.ssid;$('wifi_pass').focus()};
      e.appendChild(r);
    });
  }).catch(function(){msg('Quét thất bại','err')});
}

function doSave(){
  var p=new URLSearchParams();
  p.set('wifi_ssid',$('wifi_ssid').value.trim());
  p.set('wifi_pass',$('wifi_pass').value);
  p.set('use_static_ip',$('use_static').checked?'1':'0');
  p.set('static_ip',$('static_ip').value.trim());
  p.set('static_gw',$('static_gw').value.trim());
  p.set('static_mask',$('static_mask').value.trim());
  p.set('static_dns',$('static_dns').value.trim());
  p.set('push_url',$('push_url').value.trim());
  p.set('vflip',$('vflip').value);
  p.set('hmirror',$('hmirror').value);
  p.set('ap_ssid',$('ap_ssid').value.trim());
  p.set('ap_pass',$('ap_pass').value);
  msg('Đang lưu...');
  fetch('/api/cfg',{method:'POST',body:p}).then(function(r){return r.json()}).then(function(d){
    if(d.reboot){
      msg('✅ Đã lưu. Thiết bị đang khởi động lại... nếu đổi mạng WiFi, hãy kết nối vào mạng mới rồi mở lại trang này.','ok');
    }else{
      msg('✅ Đã lưu (không cần khởi động lại).','ok');
    }
  }).catch(function(){msg('Lưu thất bại — thử lại','err')});
}

function doReset(){
  if(!confirm('Xoá toàn bộ cấu hình và về mặc định?'))return;
  var p=new URLSearchParams();p.set('reset','1');
  msg('Đang khôi phục...');
  fetch('/api/cfg',{method:'POST',body:p}).then(function(r){return r.json()}).then(function(){
    msg('✅ Đã khôi phục mặc định, thiết bị đang khởi động lại...','ok');
  }).catch(function(){msg('Thất bại','err')});
}

function togglePv(){
  var img=$('pv');
  if($('pv_on').checked){
    img.src='//'+location.hostname+':81/stream';img.style.display='block';
  }else{
    img.removeAttribute('src');img.style.display='none';   // khong de src="" (browser se goi lai /setup)
  }
}
</script>
</body>
</html>
)SETUPHTML";
