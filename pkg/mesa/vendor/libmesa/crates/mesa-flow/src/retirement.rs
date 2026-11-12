//! Signed immutable watermarks advance only after the corresponding durable
//! transaction. The name commits to direction, epochs, flow and exclusive floor.
use super::*;
pub(super) const MARKER: &[u8] = b"libmesa/flow-retirement/2\0";
impl Node {
    pub(super) fn commit_retirement(
        &mut self,
        id: MessageId,
        out: &mut Vec<Action>,
    ) -> Result<(), Error> {
        let incoming = id.sender != self.checkpoint.local.name;
        self.controls.remove(&Work {
            flow: id.flow,
            sequence: id.sequence,
            kind: if incoming {
                ReadKind::Release
            } else {
                ReadKind::Released
            },
        });
        self.advertised.remove(&Work {
            flow: id.flow,
            sequence: id.sequence,
            kind: if incoming {
                ReadKind::Release
            } else {
                ReadKind::Released
            },
        });
        if !incoming {
            self.hints.remove(&Work {
                flow: id.flow,
                sequence: id.sequence,
                kind: ReadKind::Release,
            });
        }
        self.retire_names(id.flow, incoming, out)?;
        if incoming {
            self.bind_control(id.flow, id.sequence, ReadKind::Released, out)?;
            self.emit_control_hint(&id, true, out);
        }
        Ok(())
    }

    pub(super) fn bind_control(
        &mut self,
        flow: FlowId,
        sequence: Sequence,
        kind: ReadKind,
        out: &mut Vec<Action>,
    ) -> Result<(), Error> {
        let id = self
            .checkpoint
            .message_id(flow, sequence, kind == ReadKind::Released);
        self.core_step(
            CoreEvent::Bind {
                path: id.path(kind),
                bytes: MARKER.to_vec(),
                access: Access::Public,
            },
            out,
        )
    }
    pub(super) fn retire_names(
        &mut self,
        flow: FlowId,
        incoming: bool,
        out: &mut Vec<Action>,
    ) -> Result<(), Error> {
        let state = self.checkpoint.flow(flow).unwrap();
        let floor = if incoming {
            state.receive_floor
        } else {
            state.send_floor
        };
        let kinds = if incoming {
            [ReadKind::Receipt, ReadKind::Released]
        } else {
            [ReadKind::Message, ReadKind::Release]
        };
        for kind in kinds {
            let id = self.checkpoint.message_id(flow, floor, incoming);
            let mut prefix = id.path(kind);
            prefix.truncate(prefix.len() - 16);
            // The sender retires the just-acknowledged release, while the
            // receiver retains its current acknowledgment to repair loss.
            let mut suffix = format!("{floor:016x}").into_bytes();
            if kind == ReadKind::Release {
                suffix.push(0);
            }
            self.core_step(CoreEvent::RetireBefore { prefix, suffix }, out)?;
        }
        Ok(())
    }
}
