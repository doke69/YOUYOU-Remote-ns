# S3 PC 4.46反馈路由兼容修正

2026-11-05，源码`C:/uu/current/YOUYOU-Remote-ns`。依据离线PC消费链复核，仅改Client的TWCC media SSRC参数及诊断/测试，编解码、恢复、播放和GPU保护路径不动。

## 原因与修正

此前s3send使用media SSRC=0。当前PC 4.46固定样本的RTCP解析成功后，会把TWCC的media SSRC与登记列表比较，匹配后才调用transport反馈observer；没有零值通配分支。于是本地提交成功与PC实际处理之间存在具体兼容缺口。

`Client::pollFeedbackSending()`现使用已经过首包检查的`media.ssrc`，本机sender SSRC/CNAME不变。共享transport序号窗口及音视频/RTX观察不变，旧REMB抑制/回退不变，不更换认证身份、协议或请求档位。

诊断新增`twcc-send-media-ssrc-*`，应与`stream-rate remb-media-ssrc`所记录主视频SSRC相符。后者字段名是旧REMB统计，不能当作TWCC未开启的证明。

PC安装器为4.46.0.4716；其DLL与本机安装DLL哈希一致，且与刚固定的4.45 DLL所有文件节内容一致，因此该模块内函数证据适用于这两份样本，不扩大为整个客户端行为相同。

详细函数/偏移/证据：[PC 4.46静态报告](<%USERPROFILE%/Downloads/UU Remote NS/work/uu-protocol-4440/report/PC-446-TWCC-MEDIA-SSRC-2026-11-05.md>)。本轮没有取得PC运行时Parse/过滤/回调证据，不能宣称已确定所有低码率根因。

## 验证和产物

- 定向测试解析实际复合报文，确认PT205子包media SSRC等于合成主视频SSRC；发送ON/OFF各3/3通过。
- 完整Switch程序`0.3.75-s3ssrc`构建通过，NACP/manifest/哈希核对完成。
- 10个保护文件SHA-256一致，差异与修改前副本独立保存；未写设备，未上传。
- NRO：`C:/uu/releases/diagnostics/tv-adaptation-20261105-s3-ssrc/package/UURemote-ns.nro`
- SHA-256：`8416788204a6134e74fb5202b4e83dbda0689f8e831f863e17119062a9d8e7db`，42,769,872字节。
- 日志/差异/保护核验：同目录父级的`verification.json`、`changes.patch`和构建/测试日志。

## 测试边界

用户手动备份和复制NRO，保留配对/设置。PC保持当前4.46，以同一运动场景、相同编码/实际分辨率/FPS/频率/缓冲，先预设原画约30秒、再自定义30M约30秒，正常断开退出后取回三份连接日志。不要同时改多项来归因。

看SSRC是否对上、TWCC持续提交、无旧REMB竞争、实际帧率/延迟/画质及码率响应；不要求实际输出等于上限，也不把画质接近等同PC采用TWCC。上一轮用户准确反馈是本版预设与自定义画质接近，不是与前版比较或流畅度结论。

## 真机返回

[本轮分析](C:/uu/releases/diagnostics/tv-adaptation-20261105-s3-ssrc/device-results-20261105-030319/ANALYSIS.md)：可见SSRC匹配，4,643次提交无本地失败/预算延后/回退；720p原画窗口均值约14.8M、自定义30M约21.1M，出现积极码率响应。用户反馈貌似成功但有性能压力。日志还包含1080p，需分开；不据此称PC消费的全部动态链已证实或性能验收完成。本轮只读日志，没有修改代码/设备。
