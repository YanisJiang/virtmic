# virtmic-infer（Phase 2 假变调）

Python 推理进程：读 `Local\VirtMicInferInV1`，假变调后写 `Local\VirtMicInferOutV1`，并监听 `\\.\pipe\VirtMicInferControl` 热改半音。

## 运行

需先启动 `virtmic-engine`（由它创建 infer SHM）。

```text
cd infer
set PYTHONPATH=.
python -m virtmic_infer --semitones 4
```

热改半音（经 engine 转发）：

```text
virtmic_ctl pitch 7
```

## 测试

```text
cd infer
set PYTHONPATH=.
python -m pytest tests/test_pitch.py -v
```

无 pytest 时：

```text
python -c "from virtmic_infer.pitch import shift_semitones; import array; assert len(shift_semitones(array.array('f',[0.1]*480),4))==480"
```
