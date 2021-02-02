/// @file
///
/// Implements a controller for a Kinova Jaco arm.

#include <memory>

#include <gflags/gflags.h>

#include "drake/common/drake_assert.h"
#include "drake/common/find_resource.h"
#include "drake/common/text_logging.h"
#include "drake/lcm/drake_lcm.h"
#include "drake/geometry/scene_graph.h"
#include "drake/lcmt_jaco_command.hpp"
#include "drake/lcmt_jaco_status.hpp"
#include "drake/lcmt_robot_plan.hpp"
#include "drake/manipulation/kinova_jaco/jaco_command_sender.h"
#include "drake/manipulation/kinova_jaco/jaco_constants.h"
#include "drake/manipulation/kinova_jaco/jaco_status_receiver.h"
#include "drake/manipulation/planner/robot_plan_interpolator.h"
#include "drake/systems/analysis/simulator.h"
#include "drake/systems/controllers/pid_controller.h"
#include "drake/systems/framework/context.h"
#include "drake/systems/framework/diagram.h"
#include "drake/systems/framework/diagram_builder.h"
#include "drake/systems/lcm/lcm_publisher_system.h"
#include "drake/systems/lcm/lcm_subscriber_system.h"
#include "drake/systems/primitives/adder.h"
#include "drake/systems/primitives/demultiplexer.h"
#include "drake/systems/primitives/multiplexer.h"

DEFINE_string(urdf,
    "drake/manipulation/models/jaco_description/urdf/j2s7s300_sphere_collision.urdf",
    "Name of urdf to load");

DEFINE_int32(num_joints,
             drake::manipulation::kinova_jaco::kJacoDefaultArmNumJoints,
             "Number of joints in the arm (not including fingers)");
DEFINE_int32(num_fingers,
             drake::manipulation::kinova_jaco::kJacoDefaultArmNumFingers,
             "Number of fingers on the arm");

namespace drake {
namespace examples {
namespace kinova_jaco_arm {
namespace {
using manipulation::kinova_jaco::JacoCommandSender;
using manipulation::kinova_jaco::JacoStatusReceiver;
using manipulation::planner::RobotPlanInterpolator;

const char* const kJacoUrdf =
    "drake/manipulation/models/jaco_description/urdf/"
    "j2s7s300_sphere_collision.urdf";
const char* const kLcmStatusChannel = "KINOVA_JACO_STATUS";
const char* const kLcmCommandChannel = "KINOVA_JACO_COMMAND";
const char* const kLcmPlanChannel = "COMMITTED_ROBOT_PLAN";

int DoMain() {
  lcm::DrakeLcm lcm;
  systems::DiagramBuilder<double> builder;

  auto plan_sub = builder.AddSystem(
      systems::lcm::LcmSubscriberSystem::Make<lcmt_robot_plan>(
          kLcmPlanChannel, &lcm));

  const std::string urdf =
      (!FLAGS_urdf.empty() ? FLAGS_urdf : FindResourceOrThrow(kJacoUrdf));
  auto plan_source = builder.AddSystem<RobotPlanInterpolator>(urdf);

  builder.Connect(plan_sub->get_output_port(),
                  plan_source->get_plan_input_port());

  const int num_joints = FLAGS_num_joints;
  const int num_fingers = FLAGS_num_fingers;
  std::cout<<"num_joints:" << num_joints <<"\n";
  std::cout<<"num_fingers:" << num_fingers <<"\n";
  std::cout<<"plan_source->plant().num_positions()"<< plan_source->plant().num_positions() << "\n";
  std::cout<<"plan_source->plant().num_velocities()"<< plan_source->plant().num_velocities() << "\n";

  DRAKE_DEMAND(plan_source->plant().num_positions() ==
               num_joints + num_fingers);
  DRAKE_DEMAND(plan_source->plant().num_velocities() ==
               num_joints + num_fingers);

  // The driver is operating in joint velocity mode, so that's the
  // meaningful part of the command message we'll eventually
  // construct.  We create a pid controller which calculates
  //
  // y = kp * (q_desired - q) + kd * (v_desired - v)
  //
  // (feedback term) which we'll add to v_desired from the plan source
  // (feed forward term).
  Eigen::VectorXd jaco_kp = Eigen::VectorXd::Zero(num_joints + num_fingers);
  Eigen::VectorXd jaco_ki = Eigen::VectorXd::Zero(num_joints + num_fingers);
  Eigen::VectorXd jaco_kd = Eigen::VectorXd::Zero(num_joints + num_fingers);


  // home coords
  // 0.2112,-0.2655,0.5065  1.6476,1.1079,0.1280



  if ( num_joints == 6)
  {
    std::cout << "number if joints:"<<num_joints<<":\n";
    // taken from the kinova_ros driver j2s6s300.yaml
    jaco_kp(0) = 0;//5000;
    jaco_kp(1) = 0;//5000;
    jaco_kp(2) = 0;//5000;
    jaco_kp(3) = 0;//500;
    jaco_kp(4) = 0;//200;
    jaco_kp(5) = 0;//500;
  }
  else
  {
   // I (sam.creasey) have no idea what would be good values here.
   // This seems to work OK at the low speeds of the jaco.
   std::cout << "number if joints:"<<num_joints<<":\n";
   jaco_kp.head(num_joints).fill(10);
   jaco_kp.tail(num_fingers).fill(1);
   jaco_kd.head(num_joints).fill(1);
  }
  std::cout << "jaco_kp:\n" << jaco_kp << std::endl;
  std::cout << "jaco_ki:\n" << jaco_ki << std::endl;
  std::cout << "jaco_kd:\n" << jaco_kd << std::endl;
  // The finger velocities reported from the Jaco aren't meaningful,
  // so we shouldn't try to control based on them.
  jaco_kd.tail(num_fingers).fill(0);
  std::cout << "jaco_kd:\n" << jaco_kd << std::endl;
  auto pid_controller = builder.AddSystem<systems::controllers::PidController>(
      jaco_kp, jaco_ki, jaco_kd);

  // We'll directly fix the input to the status receiver later from our lcm
  // subscriber.
  auto status_receiver = builder.AddSystem<JacoStatusReceiver>(
      num_joints, num_fingers);

  builder.Connect(status_receiver->get_state_output_port(),
                  pid_controller->get_input_port_estimated_state());
  builder.Connect(plan_source->get_output_port(0),
                  pid_controller->get_input_port_desired_state());

  // Split the plan source into q_d (sent in the command message for
  // informational purposes) and v_d (feed forward term for control).
  auto target_demux =
      builder.AddSystem<systems::Demultiplexer>(
          (num_joints + num_fingers) * 2, num_joints + num_fingers);
  builder.Connect(plan_source->get_output_port(0),
                  target_demux->get_input_port(0));

  // Sum the outputs of the pid controller and v_d.
  auto adder = builder.AddSystem<systems::Adder>(2, num_joints + num_fingers);
  builder.Connect(pid_controller->get_output_port_control(),
                  adder->get_input_port(0));
  builder.Connect(target_demux->get_output_port(1),
                  adder->get_input_port(1));

  // Multiplex the q_d and velocity command streams back into a single
  // port.
  std::vector<int> mux_sizes(2, num_joints + num_fingers);
  auto command_mux = builder.AddSystem<systems::Multiplexer>(mux_sizes);
  builder.Connect(target_demux->get_output_port(0),
                  command_mux->get_input_port(0));
  builder.Connect(adder->get_output_port(),
                  command_mux->get_input_port(1));

  auto command_pub = builder.AddSystem(
      systems::lcm::LcmPublisherSystem::Make<lcmt_jaco_command>(
          kLcmCommandChannel, &lcm));
  auto command_sender = builder.AddSystem<JacoCommandSender>(num_joints);
  builder.Connect(command_mux->get_output_port(0),
                  command_sender->get_input_port());
  builder.Connect(command_sender->get_output_port(),
                  command_pub->get_input_port());

  auto owned_diagram = builder.Build();
  const systems::Diagram<double>* diagram = owned_diagram.get();
  systems::Simulator<double> simulator(std::move(owned_diagram));

  // Wait for the first message.
  drake::log()->info("Waiting for first lcmt_jaco_status");
  lcm::Subscriber<lcmt_jaco_status> status_sub(&lcm, kLcmStatusChannel);
  LcmHandleSubscriptionsUntil(&lcm, [&]() { return status_sub.count() > 0; });
  drake::log()->info("got my first lcmt_jaco_status");


  const lcmt_jaco_status& first_status = status_sub.message();
  DRAKE_DEMAND(first_status.num_joints == 0 ||
               first_status.num_joints == num_joints);
  DRAKE_DEMAND(first_status.num_fingers == 0 ||
               first_status.num_fingers == num_fingers);

  VectorX<double> q0 = VectorX<double>::Zero(num_joints + num_fingers);
  for (int i = 0; i < first_status.num_joints; ++i) {
    q0(i) = first_status.joint_position[i];
  }

  for (int i = 0; i < first_status.num_fingers; ++i) {
    q0(i + num_joints) = first_status.finger_position[i];
  }
  #define RAD2DEG (180.0/3.1415)
  std::cout<<"joints\n";
  std::cout<<"deg q0[0]:"<<q0[0]*RAD2DEG<<"\n";
  std::cout<<"deg q0[1]:"<<q0[1]*RAD2DEG<<"\n";
  std::cout<<"deg q0[2]:"<<q0[2]*RAD2DEG<<"\n";
  std::cout<<"deg q0[3]:"<<q0[3]*RAD2DEG<<"\n";
  std::cout<<"deg q0[4]:"<<q0[4]*RAD2DEG<<"\n";
  std::cout<<"deg q0[5]:"<<q0[5]*RAD2DEG<<"\n";
  if (num_joints == 7)
  {
   std::cout<<"deg q0[6]:"<<q0[6]*RAD2DEG<<"\n";
   std::cout<<"finger\n";
   std::cout<<"q0[7]:"<<q0[7]<<"\n";
   std::cout<<"q0[8]:"<<q0[8]<<"\n";
   std::cout<<"q0[9]:"<<q0[9]<<"\n";
  }
  else
  {
    std::cout<<"finger\n";
    std::cout<<"q0[6]:"<<q0[6]<<"\n";
    std::cout<<"q0[7]:"<<q0[7]<<"\n";
    std::cout<<"q0[8]:"<<q0[8]<<"\n";
  }
  systems::Context<double>& diagram_context = simulator.get_mutable_context();
  const double t0 = first_status.utime * 1e-6;
  diagram_context.SetTime(t0);

  std::cout<<"Controller started 0\n";
  auto& plan_source_context =
      diagram->GetMutableSubsystemContext(*plan_source, &diagram_context);
  plan_source->Initialize(t0, q0,
                          &plan_source_context.get_mutable_state());

  std::cout<<"Controller started 1\n";
  systems::Context<double>& status_context =
      diagram->GetMutableSubsystemContext(*status_receiver, &diagram_context);
  auto& status_value = status_receiver->get_input_port().FixValue(
      &status_context, first_status);

  // Run forever, using the lcmt_jaco_status message to dictate when simulation
  // time advances.  The lcmt_robot_plan message is handled whenever the next
  // lcmt_jaco_status occurs.
  std::cout<<"Controller started\n";

  while (true) {
    // Wait for an lcmt_jaco_status message.
    status_sub.clear();
    LcmHandleSubscriptionsUntil(&lcm, [&]() { return status_sub.count() > 0; });
    // Write the lcmt_jaco_status message into the context and advance.
    status_value.GetMutableData()->set_value(status_sub.message());
    const double time = status_sub.message().utime * 1e-6;
    simulator.AdvanceTo(time);
    // Force-publish the lcmt_jaco_command (via the command_pub system within
    // the diagram).
    diagram->Publish(diagram_context);
  }

  // We should never reach here.
  return EXIT_FAILURE;
}

}  // namespace
}  // namespace kinova_jaco_arm
}  // namespace examples
}  // namespace drake

int main(int argc, char* argv[]) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  return drake::examples::kinova_jaco_arm::DoMain();
}
