//! Host-selected transfer policies. No wire field or attachment type selects
//! policy. These bounded prototypes do not establish Internet congestion safety.
use crate::{Config, Error, ReadId, Tick};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RetryTimer {
    Fixed { after: Tick },
    Exponential { initial: Tick, maximum: Tick },
}
impl RetryTimer {
    /// Delay after transmission `attempt`, including the first transmission.
    pub fn delay(self, attempt: u32) -> Tick {
        match self {
            Self::Fixed { after } => after,
            Self::Exponential { initial, maximum } => initial
                .saturating_mul(
                    1u64.checked_shl(attempt.saturating_sub(1))
                        .unwrap_or(u64::MAX),
                )
                .min(maximum),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RecoveryPolicy {
    /// Includes the initial transmission. Queued interests spend no attempts.
    pub max_attempts: u32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TimerPolicy {
    /// Absolute duration from admission, including time queued behind other reads.
    pub read_timeout: Tick,
    pub retry: RetryTimer,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CongestionPolicy {
    /// Aggregate outstanding transmissions to one peer, across all reads.
    Fixed { window: usize },
    /// Add one slot per window of verified responses; halve on an expiry batch.
    Aimd { initial: usize, maximum: usize },
}
impl CongestionPolicy {
    fn initial(self) -> usize {
        match self {
            Self::Fixed { window } => window,
            Self::Aimd { initial, .. } => initial,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TransferPolicy {
    pub recovery: RecoveryPolicy,
    pub timers: TimerPolicy,
    pub congestion: CongestionPolicy,
}
impl TransferPolicy {
    /// Compatibility profile: fixed per-read window from Config, with a peer
    /// ceiling large enough for every admitted read. Use with_policy to share a
    /// smaller window or select adaptive behavior.
    pub fn fixed(config: &Config) -> Self {
        Self {
            recovery: RecoveryPolicy {
                max_attempts: config.max_attempts,
            },
            timers: TimerPolicy {
                read_timeout: config.read_timeout,
                retry: RetryTimer::Fixed {
                    after: config.retry_after,
                },
            },
            congestion: CongestionPolicy::Fixed {
                window: config.window.saturating_mul(config.max_reads),
            },
        }
    }

    pub(crate) fn validate(self, config: &Config) -> Result<(), Error> {
        let bound = config
            .window
            .checked_mul(config.max_reads)
            .ok_or(Error::InvalidConfig)?;
        let maximum = match self.congestion {
            CongestionPolicy::Fixed { window } => window,
            CongestionPolicy::Aimd { maximum, .. } => maximum,
        };
        let valid_retry = match self.timers.retry {
            RetryTimer::Fixed { after } => after > 0,
            RetryTimer::Exponential { initial, maximum } => initial > 0 && maximum >= initial,
        };
        if self.recovery.max_attempts == 0
            || self.timers.read_timeout == 0
            || !valid_retry
            || self.congestion.initial() == 0
            || self.congestion.initial() > maximum
            || maximum > bound
        {
            return Err(Error::InvalidConfig);
        }
        Ok(())
    }
}

/// Observable bounded state. Queued includes unsent and expired interests;
/// in_flight includes only emitted interests whose retry timer has not expired.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TransferState {
    pub window: usize,
    pub in_flight: usize,
    pub queued: usize,
}

pub(crate) struct Controller {
    pub window: usize,
    credit: usize,
    pub cursor: Option<ReadId>,
}
impl Controller {
    pub fn new(policy: CongestionPolicy) -> Self {
        Self {
            window: policy.initial(),
            credit: 0,
            cursor: None,
        }
    }
    pub fn verified(&mut self, policy: CongestionPolicy) {
        if let CongestionPolicy::Aimd { maximum, .. } = policy {
            self.credit += 1;
            if self.credit >= self.window {
                self.window = self.window.saturating_add(1).min(maximum);
                self.credit = 0;
            }
        }
    }
    pub fn expired(&mut self, policy: CongestionPolicy) {
        if matches!(policy, CongestionPolicy::Aimd { .. }) {
            self.window = (self.window / 2).max(1);
            self.credit = 0;
        }
    }
}
