#include "dike.hpp"
#include <algorithm>
#include <apate.hpp>
#include <bit>
#include <bits/std_abs.h>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <dlfcn.h>
#include <elf.h>
#include <future>
#include <iterator>
#include <link.h>
#include <map>
#include <memory>
#include <stdio.h>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

// RunCommand hook
std::unique_ptr< apate::declared > run_command;

void hooked_run_command( void *self, valve::user_cmd *cmd, void *helper ) {
  // ignore player commands if they haven't responded to our query
  if ( !plugin.store.contains( self ) )
    return; // original( self, cmd, helper );
  auto *entry = &plugin.store[ self ];

  static auto server =
      std::bit_cast< link_map * >( dlopen( "csgo/bin/server.so", RTLD_NOW ) );

  static auto get_fov =
      std::bit_cast< int ( * )( void * ) >( server->l_addr + OFFSET_GET_FOV );
  static auto get_default_fov = std::bit_cast< int ( * )( void * ) >(
      server->l_addr + OFFSET_GET_DEFAULT_FOV );
  static auto get_distance_adjustment = std::bit_cast< float ( * )( void * ) >(
      server->l_addr + OFFSET_FOV_ADJUST );

  const auto scoped = *std::bit_cast< bool * >(
      std::bit_cast< uintptr_t >( self ) + OFFSET_IS_SCOPED );
  const auto is_resuming_zoom = *std::bit_cast< bool * >(
      std::bit_cast< uintptr_t >( self ) + OFFSET_RESUME_ZOOM );

  const auto fov = static_cast< float >( get_fov( self ) );
  const auto default_fov = static_cast< float >( get_default_fov( self ) );
  auto distance_adjustment = get_distance_adjustment( self );

  const auto zoomed = ( fov != default_fov );

  // apply zoom sensitivity ratio to adjustment
  distance_adjustment *= entry->scaling[ scaling_variable::zoom_ratio ];

  // process snapshots to check if sensitivity might be variable during current
  // cmd. (HACK - there is 100% a better way to do this)
  if ( ( cmd->buttons & button_flags::IN_ATTACK2 ) ||
       ( cmd->buttons & button_flags::IN_ATTACK ) && zoomed ||
       is_resuming_zoom )
    goto final;

  for ( const auto snapshot : entry->snapshots ) {
    if ( snapshot.cmd.buttons & button_flags::IN_ATTACK2 )
      goto final;
    if ( snapshot.fov != fov )
      goto final;
  }

  // check for both yaw and pitch
  for ( size_t idx = 0; idx <= 1; idx++ ) {
    // get m_yaw (1) / m_pitch (2)
    const auto scaling = static_cast< scaling_variable >( idx + 1 );
    float mouse_delta =
        ( cmd->view[ idx ] - entry->snapshots.front( ).cmd.view[ idx ] ) /
        entry->scaling[ scaling ];

    // yaw is negated ( angles[yaw] -= yaw * mx )
    if ( scaling == scaling_variable::yaw )
      mouse_delta = -mouse_delta;

    // apply sensitivity and distance adjustment to delta
    mouse_delta /= entry->scaling[ scaling_variable::sensitivity ];
    if ( zoomed )
      mouse_delta /= distance_adjustment;

    float abs_delta = fabsf( mouse_delta );
    if ( std::clamp( abs_delta, LOWER_BOUND, UPPER_BOUND ) == abs_delta ) {
      float deviation = std::abs( roundf( mouse_delta ) - mouse_delta );
      if ( deviation > DEVIATION_THRESHOLD ) {
        printf(
            "%p: deviation (%.4g%%) beyond threshold (%.4g%%)\n",
            self,
            deviation * 100,
            DEVIATION_THRESHOLD * 100 );
        entry->last_violation = cmd->number;
      }
    }
  }

final:
  if ( entry->snapshots.size( ) >= 4 )
    entry->snapshots.pop_back( );
  entry->snapshots.emplace_front(
      snapshot_t { .cmd = *cmd,
                   .fov = fov,
                   .zoomed = zoomed,
                   .resume_zoom = is_resuming_zoom } );

  // "punishment" system
  // this removes the IN_ATTACK bitflag, leading to attack commands being
  // ignored
  if ( cmd->number < entry->last_violation + 16 )
    cmd->buttons &= ~button_flags::IN_ATTACK;

  // call original run_command to let server handle the command.
  run_command->get_original< void ( * )( void *, void *, void * ) >( )(
      self, cmd, helper );
}

auto dike_plugin::load(
    valve::factory factory, valve::factory server_factory )
    -> bool {
  helpers = new valve::plugin_helpers { std::bit_cast< void * >(
      factory( "ISERVERPLUGINHELPERS001", nullptr ) ) };
  return false;
};

auto dike_plugin::client_loaded( valve::edict *edict ) -> void {
  // console variables we need from the client in order to verify their input
  static std::vector< std::string > console_variables = {
    "sensitivity", "m_yaw", "m_pitch", "zoom_sensitivity_ratio_mouse"
  };
  std::unordered_map< std::string, std::shared_future< std::string > > futures;
  for ( auto const &console_variable : console_variables ) {
    auto future = helpers->query_console_variable( edict, console_variable );
    futures.insert_or_assign( console_variable, std::move( future ) );
  }

  player_entry_t entry = { };
  std::thread( [ = ]( ) mutable {
    for ( auto [ console_variable, future ] : futures ) {
      static std::map< std::string, scaling_variable > lookup = {
        { "sensitivity", scaling_variable::sensitivity },
        { "m_yaw", scaling_variable::yaw },
        { "m_pitch", scaling_variable::pitch },
        { "zoom_sensitivity_ratio_mouse", scaling_variable::zoom_ratio },
      };
      auto result = future.wait_for( std::chrono::seconds( FETCH_TIMEOUT ) );
      if ( result != std::future_status::ready )
        return; // assume client isn't sending convars on purpose, we break
                // out so we dont assign to cache, leading to player commands
                // being ignored...
      auto iter = lookup.find( console_variable );
      if ( iter != std::end( lookup ) )
        entry.scaling[ iter->second ] = std::stof( future.get( ) );
    }

    plugin.store.insert_or_assign( edict->unknown, entry );
  } ).detach( );

  // only hook once, this could be done smarter (for example, in load)
  // but this is the easiest way of getting the vmt pointer
  static bool ran = false;
  if ( !ran ) {
    auto method = std::bit_cast< void * >( &( ( *std::bit_cast< uintptr_t ** >(
        edict->unknown ) )[ RUNCOMMAND_INDEX ] ) );

    run_command = std::make_unique< apate::declared >( method );
    run_command->hook( &hooked_run_command );

    ran = true;
  }
}
