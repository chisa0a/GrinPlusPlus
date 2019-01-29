#pragma once

#include <Core/Transaction.h>
#include <Wallet/ParticipantData.h>
#include <uuid.h>

#include <stdint.h>

// A 'Slate' is passed around to all parties to build up all of the public
// transaction data needed to create a finalized transaction. Callers can pass
// the slate around by whatever means they choose, (but we can provide some
// binary or JSON serialization helpers here).
class Slate
{
private:
	// The number of participants intended to take part in this transaction
	uint64_t m_numParticipants;
	// Unique transaction/slate ID, selected by sender
	uuids::uuid m_slateId;
	// The core transaction data:
	// inputs, outputs, kernels, kernel offset
	Transaction m_transaction;
	// base amount (excluding fee)
	uint64_t m_amount;
	// fee amount
	uint64_t m_fee;
	// Block height for the transaction
	uint64_t m_blockHeight;
	// Lock height
	uint64_t m_lockHeight;
	// Participant data, each participant in the transaction will insert their public data here.
	// For now, 0 is sender and 1 is receiver, though this will change for multi-party.
	std::vector<ParticipantData> m_participantData;
};