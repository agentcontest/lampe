
#include "agent2.hpp"

#include "debug.hpp"

namespace jup {

void Mothership_complex::init(Graph* graph_) {
    graph = graph_;
    world_buffer.reset();
    sit_buffer.reset();
    sit_old_buffer.reset();
}

void Mothership_complex::on_sim_start(u8 agent, Simulation const& simulation, int sim_size) {
    if (agent == 0) {
        world_buffer.emplace_back<World>(simulation, graph, &world_buffer);
    }
    world().update(simulation, agent, &world_buffer);
}

void Mothership_complex::pre_request_action() {
    std::swap(sit_buffer, sit_old_buffer);
    sit_buffer.reset();
}

void Mothership_complex::pre_request_action(u8 agent, Percept const& perc, int perc_size) {
    if (agent == 0) {
        Situation* old = sit_old_buffer.size() ? &sit_old_buffer.get<Situation>() : nullptr;
        sit_buffer.emplace_back<Situation>(perc, old, &sit_buffer);

        // This actually only invalidates the world in the first step, unless step_init changes
        world().step_init(perc, &world_buffer);
    }
    
    sit().update(perc, agent, &sit_buffer);
    world().step_update(perc, agent, &world_buffer);
}

void Mothership_complex::on_request_action() {
    sit_diff.init(&sit_buffer);
    sit().register_arr(&sit_diff);
    
    // Flush all the old tasks out
    Situation* old = sit_old_buffer.size() ? &sit_old_buffer.get<Situation>() : nullptr;
    sit().flush_old(world(), *old, &sit_diff);
    sit_diff.apply();

    sim_buffer.reset();
    sim_buffer.append(sit_buffer);
    sim_state.init(&world(), &sim_buffer, 0, sim_buffer.size());
    
    sim_state.reset();
    sim_state.fast_forward();
    
    sim_state.create_work();
    sim_state.fix_errors();
    std::memcpy(&sit().strategy, &sim_state.orig().strategy, sizeof(sit().strategy));    

    JDBG_L < sim_state.sit().strategy.p_results() ,1;
    JDBG_L < sim_state.orig().strategy.p_tasks() ,0;
    
        /*} else {
        sim_state.reset();
        sim_state.fast_forward(sit().simulation_step);

        // These are just to make the output easier on the eyes
        for (u8 agent = 0; agent < number_of_agents; ++agent) {
            sim_state.sit().self(agent).action_type = sit().self(agent).action_type;
            sim_state.sit().self(agent).action_result = sit().self(agent).action_result;
            sim_state.sit().self(agent).task_sleep = sit().self(agent).task_sleep;
            sim_state.sit().self(agent).task_state = sit().self(agent).task_state;
            sim_state.sit().self(agent).task_index = sit().self(agent).task_index;
        }
        std::memcpy(&sim_state.sit().strategy, &sit().strategy, sizeof(sit().strategy));
        
        jdbg_diff(sim_state.sit(), sit());
        }*/
    
    /*if (sit().simulation_step == 21) {
        JDBG_L < sim_state.orig().strategy.p_results() ,0;
        JDBG_L < sim_state.orig().selves ,0;
        JDBG_L < sim_state.orig().jobs ,0;
        die(false);
        }*/
    /*
    for (u8 agent = 0; agent < number_of_agents; ++agent) {
        for (u8 i = 0; i < planning_max_tasks; ++i) {
            auto const& t = sim_state.orig().strategy.task(agent, i).task;
            auto const& r = sim_state.sit().strategy.task(agent, i).result;
            if (t.type != Task::CRAFT_ITEM and t.type != Task::CRAFT_ASSIST) continue;
            if (r.time != 80) continue;
            for (u8 j = i; j < planning_max_tasks; ++j) {
                JDBG_L < agent < sim_state.orig().strategy.task(agent, j).task ,0;
            }
            break;
        }
        }*/
    
    std::memcpy(&sit().strategy, &sim_state.sit().strategy, sizeof(sit().strategy));
}

void Mothership_complex::post_request_action(u8 agent, Buffer* into) {
    Situation* old = sit().simulation_step == 0 ? &sit() : &sit_old();
    sit().get_action(world(), *old, agent, into, &sit_diff);
    sit_diff.apply();
}


} /* end of namespace jup */
