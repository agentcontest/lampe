
#include "global.hpp"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>

#include "messages.hpp"
#include "pugixml.hpp"

namespace jup {

template <typename Range>
auto distance(Range const& range) {
	return std::distance(range.begin(), range.end());
}

struct Socket_writer: public pugi::xml_writer {
	Socket_writer(Socket* sock): sock{sock} {assert(sock);}

	void write(void const* data, std::size_t size) override {
		sock->send(Buffer_view {data, (int)size});
	}

	Socket* sock;
};
struct Buffer_writer: public pugi::xml_writer {
	Buffer_writer(Buffer* buf): buf{buf} {assert(buf);}

	void write(void const* data, std::size_t size) override {
		buf->append(data, size);
	}

	Buffer* buf;
};

static Buffer memory_for_messages;
static int additional_buffer_needed = 0;

static Buffer memory_for_strings;

void* allocate_pugi(size_t size) {
	constexpr static int align = alignof(double) > alignof(void*)
		? alignof(double) : alignof(void*);
	void* result = memory_for_messages.end();
	std::size_t space = memory_for_messages.space();
	if (std::align(align, size, result, space)) {
		memory_for_messages.resize((char*)result + size - memory_for_messages.begin());
		return result;
	} else {
		jerr << "Warning: Buffer for pugi is not big enough, need additional " << size << '\n';
		additional_buffer_needed += size;
		return malloc(size);
	}
}

void deallocate_pugi(void* ptr) {
	if (memory_for_messages.begin() > ptr or ptr >= memory_for_messages.end()) {
		free(ptr);
	}
}

void init_messages() {
	pugi::set_memory_management_functions(&allocate_pugi, &deallocate_pugi);
	memory_for_messages.reserve(150 * 1024);

	memory_for_strings.reserve(2048);
	auto& map = memory_for_strings.emplace_ref<Flat_idmap>();
	
	// Guarantee that no id maps to zero
	assert(map.get_id("", &memory_for_strings) == 0);
}

u8 get_id(Buffer_view str) {
	auto& map = memory_for_strings.get_ref<Flat_idmap>();
	return map.get_id(str, &memory_for_strings);
}

u8 get_id_from_string(Buffer_view str) {
	return memory_for_strings.get_ref<Flat_idmap>().get_id(str);
}

static constexpr double mess_lat_lon_padding = 0.05;
static bool messages_lat_lon_initialized = false;
static double mess_min_lat;
static double mess_max_lat;
static double mess_min_lon;
static double mess_max_lon;

void add_bound_point(pugi::xml_node xml_obj) {
	double lat = xml_obj.attribute("lat").as_double();
	double lon = xml_obj.attribute("lon").as_double();
	if (!messages_lat_lon_initialized) {
		mess_min_lat = lat;
		mess_max_lat = lat;
		mess_min_lon = lon;
		mess_max_lon = lon;
		messages_lat_lon_initialized = true;
	}
	if (lat < mess_min_lat) mess_min_lat = lat;
	if (lat > mess_max_lat) mess_max_lat = lat;
	if (lon < mess_min_lon) mess_min_lon = lon;
	if (lon > mess_max_lon) mess_max_lon = lon;
}

Pos get_pos(pugi::xml_node xml_obj) {
	constexpr static double pad = mess_lat_lon_padding;
	double lat = xml_obj.attribute("lat").as_double();
	double lon = xml_obj.attribute("lon").as_double();
	double lat_diff = (mess_max_lat - mess_min_lat);
	double lon_diff = (mess_max_lon - mess_min_lon);
	lat = (lat - mess_min_lat + lat_diff * pad) / (1 + 2*pad) / lat_diff;
	lon = (lon - mess_min_lon + lon_diff * pad) / (1 + 2*pad) / lon_diff;
	assert(0.0 <= lat and lat < 1.0);
	assert(0.0 <= lon and lon < 1.0);
	return Pos {(u8)(lat * 256.0), (u8)(lon * 256.0)};
}

void set_xml_pos(Pos pos, pugi::xml_node* into) {
	assert(into);

	constexpr static double pad = mess_lat_lon_padding;
	double lat_diff = (mess_max_lat - mess_min_lat);
	double lon_diff = (mess_max_lon - mess_min_lon);
	double lat = (double)pos.lat / 256.0;
	double lon = (double)pos.lon / 256.0;
	lat = lat * lat_diff * (1 + 2*pad) - lat_diff * pad + mess_min_lat;
	lon = lon * lon_diff * (1 + 2*pad) - lon_diff * pad + mess_min_lon;
	
	into->append_attribute("lat") = lat;
	into->append_attribute("lon") = lon;
}

Buffer_view get_string_from_id(u8 id) {
	auto& map = memory_for_strings.get_ref<Flat_idmap>();
	return map.get_value(id);
}

void parse_auth_response(pugi::xml_node xml_obj, Buffer* into) {
	assert(into);
	auto& mess = into->emplace_back<Message_Auth_Response>();
	auto succeeded = xml_obj.attribute("result").value();
	if (std::strcmp(succeeded, "ok") == 0) {
		mess.succeeded = true;
	} else if (std::strcmp(succeeded, "fail") == 0) {
		mess.succeeded = false;
	} else {
		assert(false);
	}
}

void parse_sim_start(pugi::xml_node xml_obj, Buffer* into) {
	assert(into);

	int prev_size = into->size();
	int space_needed = sizeof(Message_Sim_Start);
	{
		constexpr int s = sizeof(u8);
		space_needed += s + sizeof(u8) * distance(xml_obj.child("role"));
		space_needed += s;
		for (auto xml_prod: xml_obj.child("products")) {
			space_needed += s * 2 + sizeof(Product)
				+ sizeof(Item_stack) * distance(xml_prod.child("consumed"))
				+ sizeof(u8) * distance(xml_prod.child("tools"));
		}
	}
	into->reserve_space(space_needed);
	into->trap_alloc(true);
	
	auto& sim = into->emplace_back<Message_Sim_Start>().simulation;
	narrow(sim.id,           xml_obj.attribute("id")         .as_int());
	narrow(sim.seed_capital, xml_obj.attribute("seedCapital").as_int());
	narrow(sim.steps,        xml_obj.attribute("steps")      .as_int());
	sim.team = get_id(xml_obj.attribute("team").value());

	auto xml_role = xml_obj.child("role");
	sim.role.name = get_id(xml_role.attribute("name").value());
	narrow(sim.role.speed,       xml_role.attribute("speed")     .as_int());
	narrow(sim.role.max_battery, xml_role.attribute("maxBattery").as_int());
	narrow(sim.role.max_load,    xml_role.attribute("maxLoad")   .as_int());
	sim.role.tools.init(into);
	for (auto xml_tool: xml_role.children("tool")) {
		u8 name = get_id(xml_tool.attribute("name").value());
		sim.role.tools.push_back(name, into);
	}

	sim.products.init(into);
	for (auto xml_prod: xml_obj.child("products").children("product")) {
		Product prod;
		prod.name = get_id(xml_prod.attribute("name").value());
		prod.assembled = xml_prod.attribute("assembled").as_bool();
		narrow(prod.volume, xml_prod.attribute("volume").as_int());
		sim.products.push_back(prod, into);
	}
	Product* prod = sim.products.begin();
	for (auto xml_prod: xml_obj.child("products").children("product")) {
		assert(prod != sim.products.end());
		prod->consumed.init(into);
		if (auto xml_cons = xml_prod.child("consumed")) {
			for (auto xml_item: xml_cons.children("item")) {
				Item_stack stack;
				stack.item = get_id(xml_item.attribute("name").value());
				narrow(stack.amount, xml_item.attribute("amount").as_int());
				prod->consumed.push_back(stack, into);
			}
		}
		prod->tools.init(into);
		if (auto xml_tools = xml_prod.child("tools")) {
			for (auto xml_tool: xml_tools.children("item")) {
				u8 id = get_id(xml_tool.attribute("name").value());
				assert(xml_tool.attribute("amount").as_int() == 1);
				prod->tools.push_back(id, into);
			}
		}
		++prod;
	}
	assert(prod == sim.products.end());
	
	into->trap_alloc(false);
	assert(into->size() - prev_size ==  space_needed);
}

void parse_sim_end(pugi::xml_node xml_obj, Buffer* into) {
	assert(into);
	auto& mess = into->emplace_back<Message_Sim_End>();
	narrow(mess.ranking, xml_obj.attribute("ranking").as_int());
	narrow(mess.score,   xml_obj.attribute("score")  .as_int());
}

void parse_request_action(pugi::xml_node xml_perc, Buffer* into) {
	assert(into);
	auto xml_self = xml_perc.child("self");
	auto xml_team = xml_perc.child("team");

	int prev_size = into->size();
	int space_needed = sizeof(Message_Request_Action);
	{
		constexpr int s = sizeof(u8);
		space_needed += s + sizeof(Item_stack) * distance(xml_self.child("items"));
		space_needed += s + sizeof(Pos) * distance(xml_self.child("route"));
		space_needed += s + sizeof(u8) * distance(xml_team.child("jobs-taken"));
		space_needed += s + sizeof(u8) * distance(xml_team.child("jobs-posted"));
		space_needed += s + sizeof(Entity) * distance(xml_perc.child("entities"));		
		space_needed += 5 * s + sizeof(Facility) * distance(xml_perc.child("facilities"));
		space_needed += (sizeof(Charging_station) - sizeof(Facility))
			* distance(xml_perc.child("facilities").children("chargingStation"));

		space_needed += 2 * s;
		for (auto xml_job: xml_perc.child("jobs").children("auctionJob")) {
			space_needed += sizeof(Job_auction) + s
				+ sizeof(Job_item) * distance(xml_job.child("items"));
		}
		for (auto xml_job: xml_perc.child("jobs").children("pricedJob")) {
			space_needed += sizeof(Job_priced) + s
				+ sizeof(Job_item) * distance(xml_job.child("items"));
		}
	}
	
	into->reserve_space(space_needed);
	into->trap_alloc(true);

	auto& perc = into->emplace_back<Message_Request_Action>().perception;

	if (!messages_lat_lon_initialized) {
		add_bound_point(xml_self);
		for (auto i: xml_perc.child("facilities")) {
			add_bound_point(i);
		}
		for (auto i: xml_perc.child("entities")) {
			add_bound_point(i);
		}
	}

	narrow(perc.deadline, xml_perc.attribute("deadline").as_ullong());
	narrow(perc.id,       xml_perc.attribute("id")      .as_int());
	narrow(perc.simulation_step,
		   xml_perc.child("simulation").attribute("step").as_int());
	
	auto& self = perc.self;
	narrow(self.charge,     xml_self.attribute("charge")   .as_int());
	narrow(self.load,       xml_self.attribute("load")     .as_int());
	self.last_action = Action::get_id(xml_self.attribute("lastAction").value());
	self.last_action_result = Action::get_result_id(
		xml_self.attribute("lastActionResult").value());
	self.pos = get_pos(xml_self);
	char const* in_fac = xml_self.attribute("inFacility").value();
	if (std::strcmp(in_fac, "none") == 0) {
		self.in_facility = 0;
	} else {		
		self.in_facility = get_id(in_fac);
	}
	int fpos = xml_self.attribute("fPosition").as_int();
	if (fpos == -1) {
		self.f_position = -1;
	} else {
		narrow(self.f_position, fpos);
	}
	
	self.items.init(into);
	for (auto xml_item: xml_self.child("items").children("item")) {
		Item_stack item;
		item.item = get_id(xml_item.attribute("name").value());
		narrow(item.amount, xml_item.attribute("amount").as_int());
		self.items.push_back(item, into);
	}
	self.route.init(into);
	for (auto xml_node: xml_self.child("route").children("n")) {
		self.route.push_back(get_pos(xml_node), into);
	}

	auto& team = perc.team;

	team.jobs_taken.init(into);
	for (auto xml_job: xml_team.child("jobs-taken").children("job")) {
		u8 job_id = get_id(xml_job.attribute("id").value());
		team.jobs_taken.push_back(job_id, into);
	}
	team.jobs_posted.init(into);
	for (auto xml_job: xml_team.child("jobs-posted").children("job")) {
		u8 job_id = get_id(xml_job.attribute("id").value());
		team.jobs_posted.push_back(job_id, into);
	}

	perc.entities.init(into);
	for (auto xml_ent: xml_perc.child("entities").children("entity")) {
		Entity ent;
		ent.name = get_id(xml_ent.attribute("name").value());
		ent.team = get_id(xml_ent.attribute("team").value());
		ent.pos =  get_pos(xml_ent);
		ent.role = get_id(xml_ent.attribute("role").value());
		perc.entities.push_back(ent, into);
	}
	
	perc.charging_stations.init(into);
	for (auto xml_fac: xml_perc.child("facilities").children("chargingStation")) {
		Charging_station fac;
		fac.name = get_id(xml_fac.attribute("name").value());
		fac.pos = get_pos(xml_fac);
		narrow(fac.rate,  xml_fac.attribute("rate") .as_int());
		narrow(fac.price, xml_fac.attribute("price").as_int());
		narrow(fac.slots, xml_fac.attribute("slots").as_int());
		if (xml_fac.child("info")) {
			narrow(fac.q_size, xml_fac.child("info").attribute("qSize").as_int());
			assert(fac.q_size + 1);
		} else {
			fac.q_size = -1;
		}
		perc.charging_stations.push_back(fac, into);
	}
	perc.dump_locations.init(into);
	for (auto xml_fac: xml_perc.child("facilities").children("dumpLocation")) {
		Dump_location fac;
		fac.name = get_id(xml_fac.attribute("name").value());
		fac.pos = get_pos(xml_fac);
		perc.dump_locations.push_back(fac, into);
	}
	perc.shops.init(into);
	for (auto xml_fac: xml_perc.child("facilities").children("shop")) {
		Shop fac;
		fac.name = get_id(xml_fac.attribute("name").value());
		fac.pos = get_pos(xml_fac);
		perc.shops.push_back(fac, into);
	}	
	perc.storages.init(into);
	for (auto xml_fac: xml_perc.child("facilities").children("storage")) {
		Storage fac;
		fac.name = get_id(xml_fac.attribute("name").value());
		fac.pos = get_pos(xml_fac);
		perc.storages.push_back(fac, into);
	}
	perc.workshops.init(into);
	for (auto xml_fac: xml_perc.child("facilities").children("workshop")) {
		Workshop fac;
		fac.name = get_id(xml_fac.attribute("name").value());
		fac.pos = get_pos(xml_fac);
		perc.workshops.push_back(fac, into);
	}

	perc.auction_jobs.init(into);
	for (auto xml_job: xml_perc.child("jobs").children("auctionJob")) {
		Job_auction job;
		job.id      = get_id(xml_job.attribute("id")     .value());
		job.storage = get_id(xml_job.attribute("storage").value());
		narrow(job.begin,   xml_job.attribute("begin") .as_int());
		narrow(job.end,     xml_job.attribute("end")   .as_int());
		narrow(job.fine,    xml_job.attribute("fine")  .as_int());
		narrow(job.max_bid, xml_job.attribute("maxBid").as_int());
		perc.auction_jobs.push_back(job, into);
	}
	Job* job = perc.auction_jobs.begin();
	for (auto xml_job: xml_perc.child("jobs").children("auctionJob")) {
		assert(job != perc.auction_jobs.end());
		job->items.init(into);
		for (auto xml_item: xml_job.child("items").children("item")) {
			Job_item item;
			item.item = get_id(xml_item.attribute("name").value());
			narrow(item.amount,    xml_item.attribute("amount")   .as_int());
			narrow(item.delivered, xml_item.attribute("delivered").as_int());
			job->items.push_back(item, into);
		}
		++job;
	}
	assert(job == perc.auction_jobs.end());
	
	perc.priced_jobs.init(into);
	for (auto xml_job: xml_perc.child("jobs").children("pricedJob")) {
		Job_priced job;
		job.id      = get_id(xml_job.attribute("id")     .value());
		job.storage = get_id(xml_job.attribute("storage").value());
		narrow(job.begin,   xml_job.attribute("begin") .as_int());
		narrow(job.end,     xml_job.attribute("end")   .as_int());
		narrow(job.reward,  xml_job.attribute("reward").as_int());
		perc.priced_jobs.push_back(job, into);
	}
	job = perc.priced_jobs.begin();
	for (auto xml_job: xml_perc.child("jobs").children("pricedJob")) {
		assert(job != perc.priced_jobs.end());
		job->items.init(into);
		for (auto xml_item: xml_job.child("items").children("item")) {
			Job_item item;
			item.item = get_id(xml_item.attribute("name").value());
			narrow(item.amount,    xml_item.attribute("amount")   .as_int());
			narrow(item.delivered, xml_item.attribute("delivered").as_int());
			job->items.push_back(item, into);
		}
		++job;
	}
	assert(job == perc.priced_jobs.end());
		
	into->trap_alloc(false);
	assert(into->size() - prev_size ==  space_needed);
}

u8 get_next_message(Socket& sock, Buffer* into) {
	assert(into);

	memory_for_messages.reset();
	memory_for_messages.trap_alloc(false);
	memory_for_messages.reserve_space(additional_buffer_needed);
	memory_for_messages.trap_alloc(true);
	additional_buffer_needed = 0;

	do {
		sock.recv(&memory_for_messages);
		assert(memory_for_messages.size());
	} while (memory_for_messages.end()[-1] != 0);
		
	pugi::xml_document doc;
	assert(doc.load_buffer_inplace(memory_for_messages.data(), memory_for_messages.size()));
	auto xml_mess = doc.child("message");

	auto type = xml_mess.attribute("type").value();
	
	if (std::strcmp(type, "auth-response") == 0) {
		parse_auth_response(xml_mess.child("authentication"), into);
	} else if (std::strcmp(type, "sim-start") == 0) {
		parse_sim_start(xml_mess.child("simulation"), into);
	} else if (std::strcmp(type, "sim-end") == 0) {
		parse_sim_end(xml_mess.child("sim-result"), into);
	} else if (std::strcmp(type, "request-action") == 0) {
		parse_request_action(xml_mess.child("perception"), into);
	} else if (std::strcmp(type, "bye") == 0) {
		into->emplace_back<Message_Bye>();
	} else {
		assert(false);
	}
	
	auto& mess = into->get_ref<Message_Server2Client>();
	narrow(mess.timestamp, xml_mess.attribute("timestamp").as_ullong());
	return mess.type;
}

pugi::xml_node prep_message_xml(Message const& mess, pugi::xml_document* into,
								char const* type) {
	assert(into);
	auto xml_decl = into->prepend_child(pugi::node_declaration);
	xml_decl.append_attribute("version") = "1.0";
	xml_decl.append_attribute("encoding") = "UTF-8";

	auto xml_mess = into->append_child("message");
	xml_mess.append_attribute("type") = type;
	return xml_mess;
}
void send_xml_message(Socket& sock, pugi::xml_document& doc) {
	Socket_writer writer {&sock};
	doc.save(writer, "", pugi::format_default, pugi::encoding_utf8);
	sock.send({"", 1});
}

void send_message(Socket& sock, Message_Auth_Request const& mess) {
	pugi::xml_document doc;
	auto xml_mess = prep_message_xml(mess, &doc, "auth-request");
	
	auto xml_auth = xml_mess.append_child("authentication");
	xml_auth.append_attribute("username") = mess.username.c_str();
	xml_auth.append_attribute("password") = mess.password.c_str();
	
	send_xml_message(sock, doc);
}

char const* generate_action_param(Action const& action) {
	pugi::xml_document doc;
	auto param = doc.append_child("param");

	auto write_item_stack_list = [&param](Flat_array<Item_stack> const& items) {
		constexpr static int buf_len = 16;
		constexpr static char const* str1("item");
		constexpr static char const* str2("amount");
			
		memory_for_messages.reserve_space(
			buf_len + std::max(strlen(str1), strlen(str2))
		);
		char* buf = memory_for_messages.end();
		std::strcpy(buf, str1);
		for (int i = 0; i < items.size(); ++i) {
			std::snprintf(buf + strlen(str1), buf_len, "%d", i + 1);
			param.append_attribute(buf) = get_string_from_id(items[i].item).c_str();
		}
		std::strcpy(buf, str2);
		for (int i = 0; i < items.size(); ++i) {
			std::snprintf(buf + strlen(str2), buf_len, "%d", i + 1);
			param.append_attribute(buf) = items[i].amount;
		}
	};
	
	switch (action.type) {
	case Action::GOTO: assert(false); break;
	case Action::GOTO1: {
		auto const& a = (Action_Goto1 const&) action;
		param.append_attribute("facility") = get_string_from_id(a.facility).c_str();
		break;
	}
	case Action::GOTO2: {
		auto const& a = (Action_Goto2 const&) action;
		set_xml_pos(a.pos, &param);
		break;
	}
	case Action::BUY: {
		auto const& a = (Action_Buy const&) action;
		param.append_attribute("item") = get_string_from_id(a.item.item).c_str();
		param.append_attribute("amount") = a.item.amount;
		break;
	}
	case Action::GIVE: {
		auto const& a = (Action_Give const&) action;
		param.append_attribute("agent") = get_string_from_id(a.agent).c_str();
		param.append_attribute("item") = get_string_from_id(a.item.item).c_str();
		param.append_attribute("amount") = a.item.amount;
		break;
	}
	case Action::RECIEVE: {
		break;
	}
	case Action::STORE: {
		auto const& a = (Action_Store const&) action;
		param.append_attribute("item") = get_string_from_id(a.item.item).c_str();
		param.append_attribute("amount") = a.item.amount;
		break;
	}
	case Action::RETRIEVE: {
		auto const& a = (Action_Retrieve const&) action;
		param.append_attribute("item") = get_string_from_id(a.item.item).c_str();
		param.append_attribute("amount") = a.item.amount;
		break;
	}
	case Action::RETRIEVE_DELIVERED: {
		auto const& a = (Action_Retrieve_delivered const&) action;
		param.append_attribute("item") = get_string_from_id(a.item.item).c_str();
		param.append_attribute("amount") = a.item.amount;
		break;
	}
	case Action::DUMP: {
		auto const& a = (Action_Dump const&) action;
		param.append_attribute("item") = get_string_from_id(a.item.item).c_str();
		param.append_attribute("amount") = a.item.amount;
		break;
	}
	case Action::ASSEMBLE: {
		auto const& a = (Action_Assemble const&) action;
		param.append_attribute("item") = get_string_from_id(a.item).c_str();
		break;
	}
	case Action::ASSIST_ASSEMBLE: {
		auto const& a = (Action_Assist_assemble const&) action;
		param.append_attribute("assembler") = get_string_from_id(a.assembler).c_str();
		break;
	}
	case Action::DELIVER_JOB: {
		auto const& a = (Action_Deliver_job const&) action;
		param.append_attribute("job") = get_string_from_id(a.job).c_str();
		break;
	}
	case Action::CHARGE: {
		break;
	}
	case Action::BID_FOR_JOB: {
		auto const& a = (Action_Bid_for_job const&) action;
		param.append_attribute("job") = get_string_from_id(a.job).c_str();
		param.append_attribute("price") = a.price;
		break;
	}
	case Action::POST_JOB1: {
		auto const& a = (Action_Post_job1 const&) action;
		param.append_attribute("type") = "auction";
		param.append_attribute("max_price") = a.max_price;
		param.append_attribute("fine") = a.fine;
		param.append_attribute("active_steps") = a.active_steps;
		param.append_attribute("auction_steps") = a.auction_steps;
		param.append_attribute("storage") = get_string_from_id(a.storage).c_str();
		write_item_stack_list(a.items);
		break;
	}
	case Action::POST_JOB2: {
		auto const& a = (Action_Post_job2 const&) action;
		param.append_attribute("type") = "priced";
		param.append_attribute("price") = a.price;
		param.append_attribute("active_steps") = a.active_steps;
		param.append_attribute("storage") = get_string_from_id(a.storage).c_str();
		write_item_stack_list(a.items);
		break;
	}
	case Action::CALL_BREAKDOWN_SERVICE: {
		break;
	}
	case Action::CONTINUE: {
		break;
	}
	case Action::SKIP: {
		break;
	}
	case Action::ABORT: {
		break;
	}
	default: assert(false); break;
	}

	char* start = memory_for_messages.end();
	Buffer_writer writer {&memory_for_messages};
	param.print(writer, "", pugi::format_raw);
	memory_for_messages.emplace_back<char>('\0');
	char* end = memory_for_messages.end();	  
	assert(end[-1] == 0);
	static constexpr char const* str1 = "<param ";
	assert(strncmp(start, str1, strlen(str1)) == 0);
	start += strlen(str1);
	end -= strlen("/>") + 1;
	assert(strcmp(end, "/>") == 0);
	*end = 0;
	return start;
}

void send_message(Socket& sock, Message_Action const& mess) {
	pugi::xml_document doc;
	auto xml_mess = prep_message_xml(mess, &doc, "action");
	
	auto xml_auth = xml_mess.append_child("action");
	xml_auth.append_attribute("id") = mess.id;
	xml_auth.append_attribute("type") = Action::get_name(mess.action->type);
	xml_auth.append_attribute("param") = generate_action_param(*mess.action);
	
	send_xml_message(sock, doc);
}

} /* end of namespace jup */

int messagesMain() {
	jup::Socket_context context;
	jup::Socket sock {"localhost", "12300"};
	if (!sock) { return 1; }
	
	jup::init_messages();

	jup::send_message(sock, jup::Message_Auth_Request {"a1", "1"});

	jup::Buffer buffer;
	get_next_message(sock, &buffer);
	auto& mess1 = buffer.get_ref<jup::Message_Auth_Response>();
	if (mess1.succeeded) {
		jup::jout << "Conected to server. Please start the simulation.\n";
		jup::jout.flush();
	} else {
		jup::jout << "Invalid authentification.\n";
		return 1;
	}

	buffer.reset();
	get_next_message(sock, &buffer);
	auto& mess2 = buffer.get_ref<jup::Message_Sim_Start>();
	jup::jout << "Got the simulation. Steps: "
			  << mess2.simulation.steps << '\n';

	while (true) {
		buffer.reset();
		assert(get_next_message(sock, &buffer) == jup::Message::REQUEST_ACTION);
		auto& mess = buffer.get_ref<jup::Message_Request_Action>();
		jup::jout << "Got the message request. Step: "
				  << mess.perception.simulation_step << '\n';

		auto& answ = jup::memory_for_messages.emplace_back<jup::Message_Action>
			( mess.perception.id, jup::Action_Skip {}, &jup::memory_for_messages );
		jup::send_message(sock, answ);
	}
	
    return 0;
}

