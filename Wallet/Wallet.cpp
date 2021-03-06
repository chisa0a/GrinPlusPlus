#include "Wallet.h"
#include "WalletRefresher.h"
#include "Keychain/KeyChain.h"
#include <unordered_set>

Wallet::Wallet(const Config& config, const INodeClient& nodeClient, IWalletDB& walletDB, const std::string& username, KeyChainPath&& userPath)
	: m_config(config), m_nodeClient(nodeClient), m_walletDB(walletDB), m_username(username), m_userPath(std::move(userPath))
{

}

Wallet* Wallet::LoadWallet(const Config& config, const INodeClient& nodeClient, IWalletDB& walletDB, const std::string& username)
{
	KeyChainPath userPath = KeyChainPath::FromString("m/0/0"); // TODO: Need to lookup actual account
	return new Wallet(config, nodeClient, walletDB, username, std::move(userPath));
}

WalletSummary Wallet::GetWalletSummary(const SecureVector& masterSeed)
{
	uint64_t awaitingConfirmation = 0;
	uint64_t immature = 0;
	uint64_t locked = 0;
	uint64_t spendable = 0;

	const uint64_t lastConfirmedHeight = m_nodeClient.GetChainHeight();
	const std::vector<OutputData> outputs = RefreshOutputs(masterSeed);
	for (const OutputData& outputData : outputs)
	{
		const EOutputStatus status = outputData.GetStatus();
		if (status == EOutputStatus::LOCKED)
		{
			locked += outputData.GetAmount();
		}
		else if (status == EOutputStatus::SPENDABLE)
		{
			spendable += outputData.GetAmount();
		}
		else if (status == EOutputStatus::IMMATURE)
		{
			immature += outputData.GetAmount();
		}
		else if (status == EOutputStatus::NO_CONFIRMATIONS)
		{
			awaitingConfirmation += outputData.GetAmount();
		}
	}

	const uint64_t total = awaitingConfirmation + immature + spendable;
	std::vector<WalletTx> transactions = m_walletDB.GetTransactions(m_username, masterSeed);
	return WalletSummary(lastConfirmedHeight, m_config.GetWalletConfig().GetMinimumConfirmations(), total, awaitingConfirmation, immature, locked, spendable, std::move(transactions));
}

std::vector<WalletTx> Wallet::GetTransactions(const SecureVector& masterSeed)
{
	return m_walletDB.GetTransactions(m_username, masterSeed);
}

std::vector<OutputData> Wallet::RefreshOutputs(const SecureVector& masterSeed)
{
	return WalletRefresher(m_config, m_nodeClient, m_walletDB).RefreshOutputs(m_username, masterSeed);
}

bool Wallet::AddRestoredOutputs(const SecureVector& masterSeed, const std::vector<OutputData>& outputs)
{
	std::vector<WalletTx> transactions;
	transactions.reserve(outputs.size());

	for (const OutputData& output : outputs)
	{
		const uint32_t walletTxId = GetNextWalletTxId();
		EWalletTxType type = EWalletTxType::RECEIVED;
		if (output.GetOutput().GetFeatures() == EOutputFeatures::COINBASE_OUTPUT)
		{
			type = EWalletTxType::COINBASE;
		}

		const std::chrono::system_clock::time_point creationTime = std::chrono::system_clock::now(); // TODO: Determine this
		const std::optional<std::chrono::system_clock::time_point> confirmationTimeOpt = std::make_optional<std::chrono::system_clock::time_point>(std::chrono::system_clock::now()); // TODO: Determine this

		WalletTx walletTx(walletTxId, type, std::nullopt, creationTime, confirmationTimeOpt, output.GetBlockHeight(), output.GetAmount(), 0, std::nullopt, std::nullopt);
		transactions.emplace_back(std::move(walletTx));
	}

	return AddWalletTxs(masterSeed, transactions) && m_walletDB.AddOutputs(m_username, masterSeed, outputs);
}

uint64_t Wallet::GetRefreshHeight() const
{
	return m_walletDB.GetRefreshBlockHeight(m_username);
}

bool Wallet::SetRefreshHeight(const uint64_t blockHeight)
{
	return m_walletDB.UpdateRefreshBlockHeight(m_username, blockHeight);
}

uint64_t Wallet::GetRestoreLeafIndex() const
{
	return m_walletDB.GetRestoreLeafIndex(m_username);
}

bool Wallet::SetRestoreLeafIndex(const uint64_t lastLeafIndex)
{
	return m_walletDB.UpdateRestoreLeafIndex(m_username, lastLeafIndex);
}

uint32_t Wallet::GetNextWalletTxId()
{
	return m_walletDB.GetNextTransactionId(m_username);
}

bool Wallet::AddWalletTxs(const SecureVector& masterSeed, const std::vector<WalletTx>& transactions)
{
	bool success = true;
	for (const WalletTx& transaction : transactions)
	{
		success = success && m_walletDB.AddTransaction(m_username, masterSeed, transaction);
	}

	return success;
}

std::vector<OutputData> Wallet::GetAllAvailableCoins(const SecureVector& masterSeed) const
{
	const KeyChain keyChain = KeyChain::FromSeed(m_config, masterSeed);

	std::vector<OutputData> coins;

	std::vector<Commitment> commitments;
	const std::vector<OutputData> outputs = WalletRefresher(m_config, m_nodeClient, m_walletDB).RefreshOutputs(m_username, masterSeed);
	for (const OutputData& output : outputs)
	{
		if (output.GetStatus() == EOutputStatus::SPENDABLE)
		{
			SecretKey blindingFactor = *keyChain.DerivePrivateKey(output.GetKeyChainPath(), output.GetAmount());
			coins.emplace_back(OutputData(output));
		}
	}

	return coins;
}

OutputData Wallet::CreateBlindedOutput(const SecureVector& masterSeed, const uint64_t amount)
{
	const KeyChain keyChain = KeyChain::FromSeed(m_config, masterSeed);

	KeyChainPath keyChainPath = m_walletDB.GetNextChildPath(m_username, m_userPath);
	SecretKey blindingFactor = *keyChain.DerivePrivateKey(keyChainPath, amount);
	std::unique_ptr<Commitment> pCommitment = Crypto::CommitBlinded(amount, BlindingFactor(blindingFactor.GetBytes())); // TODO: Creating a BlindingFactor here is unsafe. The memory may not get cleared.
	if (pCommitment != nullptr)
	{
		std::unique_ptr<RangeProof> pRangeProof = keyChain.GenerateRangeProof(keyChainPath, amount, *pCommitment, blindingFactor);
		if (pRangeProof != nullptr)
		{
			TransactionOutput transactionOutput(EOutputFeatures::DEFAULT_OUTPUT, Commitment(*pCommitment), RangeProof(*pRangeProof));
			
			return OutputData(std::move(keyChainPath), std::move(blindingFactor), std::move(transactionOutput), amount, EOutputStatus::NO_CONFIRMATIONS);
		}
	}

	throw std::exception(); // TODO: Determine exception type
}

bool Wallet::SaveOutputs(const SecureVector& masterSeed, const std::vector<OutputData>& outputsToSave)
{
	return m_walletDB.AddOutputs(m_username, masterSeed, outputsToSave);
}

std::unique_ptr<SlateContext> Wallet::GetSlateContext(const uuids::uuid& slateId, const SecureVector& masterSeed) const
{
	return m_walletDB.LoadSlateContext(m_username, masterSeed, slateId);
}

bool Wallet::SaveSlateContext(const uuids::uuid& slateId, const SecureVector& masterSeed, const SlateContext& slateContext)
{
	return m_walletDB.SaveSlateContext(m_username, masterSeed, slateId, slateContext);
}

bool Wallet::LockCoins(const SecureVector& masterSeed, std::vector<OutputData>& coins)
{
	for (OutputData& coin : coins)
	{
		coin.SetStatus(EOutputStatus::LOCKED);
	}

	return m_walletDB.AddOutputs(m_username, masterSeed, coins);
}

std::unique_ptr<WalletTx> Wallet::GetTxById(const SecureVector& masterSeed, const uint32_t walletTxId)
{
	std::vector<WalletTx> transactions = m_walletDB.GetTransactions(m_username, masterSeed);
	for (WalletTx& walletTx : transactions)
	{
		if (walletTx.GetId() == walletTxId)
		{
			return std::make_unique<WalletTx>(WalletTx(walletTx));
		}
	}

	return std::unique_ptr<WalletTx>(nullptr);
}

std::unique_ptr<WalletTx> Wallet::GetTxBySlateId(const SecureVector& masterSeed, const uuids::uuid& slateId)
{
	std::vector<WalletTx> transactions = m_walletDB.GetTransactions(m_username, masterSeed);
	for (WalletTx& walletTx : transactions)
	{
		if (walletTx.GetSlateId().has_value() && walletTx.GetSlateId().value() == slateId)
		{
			return std::make_unique<WalletTx>(WalletTx(walletTx));
		}
	}

	return std::unique_ptr<WalletTx>(nullptr);
}


bool Wallet::CancelWalletTx(const SecureVector& masterSeed, WalletTx& walletTx)
{
	const EWalletTxType type = walletTx.GetType();
	if (type == EWalletTxType::RECEIVING_IN_PROGRESS)
	{
		walletTx.SetType(EWalletTxType::RECEIVED_CANCELED);
	}
	else if (type == EWalletTxType::SENDING_STARTED)
	{
		walletTx.SetType(EWalletTxType::SENT_CANCELED);
	}
	else
	{
		return false;
	}

	std::optional<Transaction> transactionOpt = walletTx.GetTransaction();
	if (transactionOpt.has_value())
	{
		std::unordered_set<Commitment> commitments;
		for (const TransactionOutput& output : transactionOpt.value().GetBody().GetOutputs())
		{
			commitments.insert(output.GetCommitment());
		}

		for (const TransactionInput& input : transactionOpt.value().GetBody().GetInputs())
		{
			commitments.insert(input.GetCommitment());
		}

		std::vector<OutputData> outputsToUpdate;

		std::vector<OutputData> outputs = m_walletDB.GetOutputs(m_username, masterSeed);
		for (OutputData& output : outputs)
		{
			if (commitments.find(output.GetOutput().GetCommitment()) != commitments.end())
			{
				const EOutputStatus status = output.GetStatus();
				if (status == EOutputStatus::NO_CONFIRMATIONS)
				{
					output.SetStatus(EOutputStatus::CANCELED);
					outputsToUpdate.push_back(output);
				}
				else if (status == EOutputStatus::LOCKED)
				{
					output.SetStatus(EOutputStatus::SPENDABLE);
					outputsToUpdate.push_back(output);
				}
				else
				{
					return false;
				}
			}
		}

		if (!m_walletDB.AddOutputs(m_username, masterSeed, outputsToUpdate))
		{
			return false;
		}
	}

	return m_walletDB.AddTransaction(m_username, masterSeed, walletTx);
}