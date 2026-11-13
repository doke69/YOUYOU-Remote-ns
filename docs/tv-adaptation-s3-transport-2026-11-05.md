# S3发送前置：transport绑定及REMB共存核对

2026-11-05；源码：`C:/uu/current/YOUYOU-Remote-ns`。

## 本轮修改

依赖覆盖`src/impl/peerconnection.cpp`的全局媒体反馈回调保存创建回调时的弱transport引用。调用时检查peer/transport存在、未关闭及当前transport仍相同，随后只对这个保存的transport调用`sendMedia()`。拒绝仍经已有异常接口传给发送器，没有增加catch层。

此前只保存弱peer、调用时读取当前transport，保存下来的旧反馈回调存在转到替换transport的代码路径。本次落实已批准S3“不跨代转发”要求，没有改变收包认证、Track分发、组帧恢复、解码、播放或GPU。默认无全局handler时，不经过新增检查。

这里保证回调创建后的发送对象不会重新定向；不证明全部接收入站/transport替换交错已验证。已开始的旧transport提交仍可结束，不是全在飞取消屏障，也不声称真机已复现此故障。

覆盖源和external源同步，manifest更新；`patch_deps.py`核对34项覆盖通过、应用0项，不覆盖未知修改。

## REMB共存的已验证条件

`RtcpReceivingSession`保存成功请求的`mRequestedBitrate`，后续收到SR会重发。只让Client跳过主动REMB调用，不能阻止这一路。

已有`requestBitrate(0)`可以清除保存的目标，不发“0带宽REMB”。新增真实接收会话内存回归：先发25M→清零→连续3个SR，逐包确认只剩RR+SDES且本机身份保持→恢复12M，确认复合REMB恢复。测试通过，没有使用网络。

本轮未实际切换Client反馈模式，旧REMB保持。未来顺序必须是：协商及首包确认→清除旧REMB并确认本地接受→启动TWCC；停止并分离TWCC后才恢复兼容REMB。清除失败不启动，不能让两路竞争。真实PC互通仍待验证。

## 身份和预算边界

- 视频已有每连接`localRtcpSsrc_`/`localRtcpCname_`，新复合反馈应复用，不能另造不一致身份。
- 音频及原PLI/NACK仍有历史身份路径，本轮未改写；Strict恢复/补包没有被TWCC预算节流。
- `SharedRtcpBudget`仅覆盖接入者；原RR/NACK未全部接入，不能声称总RTCP预算已受控。
- 未启用Client发送、未假装支持02请求反馈、未替换传统RBE算法。

## 验证与交付

归档：`C:/uu/releases/diagnostics/tv-adaptation-20261105-s3-transport/`。

- host发送ON定向测试3/3通过（仅内存回调），恢复OFF后3/3通过；新增REMB切换回归1/1通过。
- Switch完整观察配置`0.3.75-s3obs`、默认配置`0.3.75-s3base`构建通过，二者发送均OFF。
- `verification.json`记录NACP、构建开关、产物哈希和10个保护文件核对；`changes.patch`仅保存本轮代码变化，`before/`保留修改前文件。
- 未部署、未采集、未发布；旧包保留，实际互通、开销和码率收益未验收。

S3未整体完成。下一步先在真实最终协商上验证01序号和有效到包时间，再接Client发送生命周期及上述互斥/回退顺序。没有运行证据，不移除构建保护或猜扩展ID来打开发送。
