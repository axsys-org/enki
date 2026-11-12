use crate::{
    obj,
    object::*,
    value::{err, nums, Result, Value as V},
};
use std::sync::{Arc, Mutex};
fn limits(d: &V) -> Result<mesa_store::Limits> {
    if d.list()?.is_empty() {
        Ok(mesa_store::Limits::default())
    } else {
        Ok(mesa_store::Limits {
            checkpoint_bytes: d.at(0)?.size()?,
            message_bytes: d.at(1)?.size()?,
            delivery_records: d.at(2)?.size()?,
            delivery_bytes: d.at(3)?.size()?,
        })
    }
}
pub fn dispatch(op: &str, h: &mut Handles, d: &V) -> Result<Output> {
    Ok(match op {
        "StoreCreate" | "StoreOpen" => {
            let c = obj!(h, 0, Checkpoint);
            let path = d.at(0)?.str()?;
            let l = limits(d.at(1)?)?;
            let s = if op == "StoreCreate" {
                mesa_store::Store::create(path, c, l)
            } else {
                mesa_store::Store::open(path, c, l)
            }
            .map_err(err)?;
            output(Object::Store(Arc::new(Mutex::new(Session {
                prepared: None,
                store: Box::new(s),
                generation: 0,
            }))))
        }
        "StoreInspect" => output(Object::Snapshot(
            mesa_store::Store::inspect(d.at(0)?.str()?, limits(d.at(1)?)?).map_err(err)?,
        )),
        "StoreIncarnation" => {
            let s = obj!(h, 0, Store).lock().map_err(err)?;
            if s.prepared.is_some() {
                return Err("store has an active transaction".into());
            }
            data(V::num(s.store.incarnation()))
        }
        "StoreCheckpoint" => {
            let s = obj!(h, 0, Store).lock().map_err(err)?;
            if s.prepared.is_some() {
                return Err("store has an active transaction".into());
            }
            output(Object::Checkpoint(s.store.checkpoint().map_err(err)?))
        }
        "StoreCommit" | "StorePrepare" => {
            let request = obj!(h, 1, Commit).clone();
            let session = obj!(h, 0, Store).clone();
            let mut s = session.lock().map_err(err)?;
            if s.prepared.is_some() {
                return Err("store has an active transaction".into());
            }
            if op == "StoreCommit" {
                output(Object::Token(s.store.commit(&request).map_err(err)?))
            } else {
                let generation = s
                    .generation
                    .checked_add(1)
                    .ok_or("transaction generation exhausted")?;
                // SAFETY: the Box allocation is stable. While Prepared exists, all Store
                // access is rejected. Session drops Prepared before Store; Transaction holds
                // an Arc to Session, so closing its parent handle cannot invalidate it.
                let store: &'static mut mesa_store::Store =
                    unsafe { &mut *(&mut *s.store as *mut mesa_store::Store) };
                s.prepared = Some(store.prepare(&request).map_err(err)?);
                s.generation = generation;
                drop(s);
                output(Object::Transaction(Transaction {
                    session,
                    generation,
                }))
            }
        }
        "TransactionCommit" => {
            let Object::Transaction(tx) = take_kind(h, 0, "Transaction")? else {
                unreachable!()
            };
            let token = {
                let mut s = tx.session.lock().map_err(err)?;
                if s.generation != tx.generation {
                    return Err("stale transaction".into());
                }
                s.prepared
                    .take()
                    .ok_or("transaction already finished")?
                    .commit()
                    .map_err(err)?
            };
            output(Object::Token(token))
        }
        "TransactionRollback" => {
            let Object::Transaction(tx) = take_kind(h, 0, "Transaction")? else {
                unreachable!()
            };
            drop(tx);
            data(V::num(0))
        }
        "SnapshotCheckpoint" => output(Object::Checkpoint(obj!(h, 0, Snapshot).checkpoint.clone())),
        "SnapshotCounts" => {
            let s = obj!(h, 0, Snapshot);
            data(nums([
                s.deliveries.len() as u128,
                s.settlements.len() as u128,
            ]))
        }
        "SnapshotDelivery" => {
            let s = obj!(h, 0, Snapshot);
            let x = s.deliveries.get(d.at(0)?.size()?).ok_or("delivery index")?;
            data(V::row([
                V::num(x.revision as u128),
                crate::flow::id_value(&x.id),
                V::bytes(&x.bytes),
                crate::flow::outcome_value(x.outcome),
            ]))
        }
        "SnapshotSettlement" => {
            let s = obj!(h, 0, Snapshot);
            let x = s
                .settlements
                .get(d.at(0)?.size()?)
                .ok_or("settlement index")?;
            data(V::row([
                V::num(x.revision as u128),
                crate::flow::id_value(&x.id),
                crate::flow::outcome_value(x.outcome),
            ]))
        }
        _ => return Err("unknown storage operation".into()),
    })
}
