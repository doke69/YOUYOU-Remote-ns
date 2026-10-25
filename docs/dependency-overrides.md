# 依赖源覆盖与可复现性

依赖归档来自scripts/sources.lock.json，fetch_deps.py逐个校验哈希。scripts/dependency-overrides/manifest.json记录31个修改文件的原始和修改后内容哈希；文本哈希将CRLF标准化为LF。

这些覆盖是0.3.44源码快照锁定的依赖源，保留原版权/许可证头，包括libdatachannel、libjuice、usrsctp及libSRTP构建适配。不要把所有文件统称为MPL：每份文件按自身原有许可。该描述表示快照中的锁定依赖，不表示所有依赖都由本次文档维护重新下载或构建。

patch_deps.py先检查所有锁定归档标记、覆盖文件哈希及目标文件是否是原始/已应用版本，再进行写入；发现未知修改就拒绝覆盖。再次执行应显示applied 0 files。不要把不同实验宏、历史SDK或旧对象混入当前构建。

公开前已在干净归档上执行并检查重复运行，所有覆盖与实际开发源规范化后一致。此验证证明源文件复现，不等于全新系统上端到端编译或网络互通都已验证。
