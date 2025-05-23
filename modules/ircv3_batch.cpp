/*
 * InspIRCd -- Internet Relay Chat Daemon
 *
 *   Copyright (C) 2021 Dominic Hamon
 *   Copyright (C) 2018-2022 Sadie Powell <sadie@witchery.services>
 *   Copyright (C) 2018 Attila Molnar <attilamolnar@hush.com>
 *
 * This file is part of InspIRCd.  InspIRCd is free software: you can
 * redistribute it and/or modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation, version 2.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include "inspircd.h"
#include "modules/cap.h"
#include "modules/ircv3_batch.h"

class BatchMessage final
	: public ClientProtocol::Message
{
public:
	BatchMessage(const IRCv3::Batch::Batch& batch, bool start)
		: ClientProtocol::Message("BATCH", ServerInstance->Config->GetServerName())
	{
		char c = (start ? '+' : '-');
		PushParam(std::string(1, c) + batch.GetRefTagStr());
		if ((start) && (!batch.GetType().empty()))
			PushParamRef(batch.GetType());
	}
};

/** Extra structure allocated only for running batches, containing objects only relevant for
 * that specific run of the batch.
 */
struct IRCv3::Batch::BatchInfo final
{
	/** List of users that have received the batch start message
	 */
	std::vector<LocalUser*> users;
	BatchMessage startmsg;
	ClientProtocol::Event startevent;
	BatchMessage endmsg;
	ClientProtocol::Event endevent;

	BatchInfo(ClientProtocol::EventProvider& protoevprov, IRCv3::Batch::Batch& b)
		: startmsg(b, true)
		, startevent(protoevprov, startmsg)
		, endmsg(b, false)
		, endevent(protoevprov, endmsg)
	{
	}
};

class IRCv3::Batch::ManagerImpl final
	: public Manager
{
	typedef std::vector<Batch*> BatchList;

	Cap::Capability cap;
	ClientProtocol::EventProvider protoevprov;
	IntExtItem batchbits;
	BatchList active_batches;
	bool unloading = false;

	bool ShouldSendTag(LocalUser* user, const ClientProtocol::MessageTagData& tagdata) override
	{
		if (!cap.IsEnabled(user))
			return false;

		Batch& batch = *static_cast<Batch*>(tagdata.provdata);
		// Check if this is the first message the user is getting that is part of the batch
		const intptr_t bits = batchbits.Get(user);
		if (!(bits & batch.GetBit()))
		{
			// Send the start batch command ("BATCH +reftag TYPE"), remember the user so we can send them a
			// "BATCH -reftag" message later when the batch ends and set the flag we just checked so this is
			// only done once per user per batch.
			batchbits.Set(user, (bits | batch.GetBit()));
			batch.batchinfo->users.push_back(user);
			user->Send(batch.batchinfo->startevent);
		}

		return true;
	}

	unsigned int NextFreeId() const
	{
		if (active_batches.empty())
			return 0;
		return active_batches.back()->GetId()+1;
	}

public:
	ManagerImpl(Module* mod)
		: Manager(mod)
		, cap(mod, "batch")
		, protoevprov(mod, "BATCH")
		, batchbits(mod, "batchbits", ExtensionType::USER)
	{
	}

	void Init()
	{
		// Set batchbits to 0 for all users in case we were reloaded and the previous, now meaningless,
		// batchbits are set on users
		for (auto* user : ServerInstance->Users.GetLocalUsers())
			batchbits.Unset(user);
	}

	void Shutdown()
	{
		unloading = true;
		while (!active_batches.empty())
			ManagerImpl::End(*active_batches.back());
	}

	void RemoveFromAll(LocalUser* user)
	{
		const intptr_t bits = batchbits.Get(user);

		// User is quitting, remove them from all lists
		for (const auto& batch : active_batches)
		{
			// Check the bit first to avoid list scan in case they're not on the list
			if ((bits & batch->GetBit()) != 0)
				stdalgo::vector::swaperase(batch->batchinfo->users, user);
		}
	}

	void Start(Batch& batch) override
	{
		if (unloading)
			return;

		if (batch.IsRunning())
			return; // Already started, don't start again

		const size_t id = NextFreeId();
		if (id >= MAX_BATCHES)
			return;

		// This cast is safe thanks to the clamping above.
		batch.Setup(static_cast<unsigned int>(id));
		// Set the manager field which Batch::IsRunning() checks and is also used by AddToBatch()
		// to set the message tag
		batch.manager = this;
		batch.batchinfo = new IRCv3::Batch::BatchInfo(protoevprov, batch);
		batch.batchstartmsg = &batch.batchinfo->startmsg;
		batch.batchendmsg = &batch.batchinfo->endmsg;
		active_batches.push_back(&batch);
	}

	void End(Batch& batch) override
	{
		if (!batch.IsRunning())
			return;

		// Mark batch as stopped
		batch.manager = nullptr;

		BatchInfo& batchinfo = *batch.batchinfo;
		// Send end batch message to all users who got the batch start message and unset bit so it can be reused
		for (const auto& user : batchinfo.users)
		{
			user->Send(batchinfo.endevent);
			batchbits.Set(user, batchbits.Get(user) & ~batch.GetBit());
		}

		// erase() not swaperase because the reftag generation logic depends on the order of the elements
		std::erase(active_batches, &batch);
		delete batch.batchinfo;
		batch.batchinfo = nullptr;
	}
};

class ModuleIRCv3Batch final
	: public Module
{
private:
	IRCv3::Batch::ManagerImpl manager;

public:
	ModuleIRCv3Batch()
		: Module(VF_VENDOR, "Provides the IRCv3 batch client capability.")
		, manager(this)
	{
	}

	void init() override
	{
		manager.Init();
	}

	void OnUnloadModule(Module* mod) override
	{
		if (mod == this)
			manager.Shutdown();
	}

	void OnUserDisconnect(LocalUser* user) override
	{
		// Remove the user from all internal lists
		manager.RemoveFromAll(user);
	}
};

MODULE_INIT(ModuleIRCv3Batch)
