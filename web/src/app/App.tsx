import { useEffect, useMemo, useState } from "react";
import { api, type Agent, type AgentEvent, type AgentRun, type Repository, type Task, type VerificationRun } from "./api";
import { connectEvents } from "./events";
import { Layout } from "../components/Layout";

export function App() {
  const [repository, setRepository] = useState<Repository>();
  const [tasks, setTasks] = useState<Task[]>([]);
  const [agents, setAgents] = useState<Agent[]>([]);
  const [selectedTask, setSelectedTask] = useState<Task>();
  const [runs, setRuns] = useState<AgentRun[]>([]);
  const [events, setEvents] = useState<AgentEvent[]>([]);
  const [diff, setDiff] = useState("");
  const [verification, setVerification] = useState<VerificationRun>();
  const [selectedAgent, setSelectedAgent] = useState("shell");
  const [prompt, setPrompt] = useState("");
  const [taskPrompt, setTaskPrompt] = useState("");
  const [drawer, setDrawer] = useState<"review" | "graphs" | "activity">();
  const [busy, setBusy] = useState("Connecting to daemon…");
  const [error, setError] = useState("");

  const currentRun = runs.at(-1);
  const currentEvents = useMemo(() => events.filter((event) => !currentRun || event.runId === currentRun.id), [events, currentRun]);

  useEffect(() => {
    Promise.all([api.repository(), api.tasks(), api.agents(), api.changes()])
      .then(([repo, loadedTasks, loadedAgents, changes]) => {
        setRepository(repo);
        setTasks(loadedTasks);
        setSelectedTask(loadedTasks[0]);
        setAgents(loadedAgents);
        setDiff(changes.diff);
        setBusy("Ready");
        if (loadedAgents.find((agent) => agent.available)?.name) setSelectedAgent(loadedAgents.find((agent) => agent.available)!.name);
      })
      .catch((reason: Error) => { setError(reason.message); setBusy("Daemon unavailable"); });
    return connectEvents((event) => {
      setEvents((current) => [...current.slice(-499), event]);
      if (event.type === "agent.finished" || event.type === "agent.failed") setBusy(event.type === "agent.failed" ? "Agent failed" : "Ready");
    });
  }, []);

  useEffect(() => {
    if (!selectedTask) return;
    Promise.all([api.events(selectedTask.id), api.changes()]).then(([loadedEvents, changes]) => { setEvents(loadedEvents); setDiff(changes.diff); });
  }, [selectedTask]);

  async function createTask() {
    if (!taskPrompt.trim()) return;
    try {
      const task = await api.createTask(taskPrompt.trim());
      setTasks((current) => [task, ...current]);
      setSelectedTask(task);
      setTaskPrompt("");
    } catch (reason) { setError(reason instanceof Error ? reason.message : "Could not create task"); }
  }

  async function launch() {
    if (!selectedTask) return;
    setBusy(`Starting ${selectedAgent}…`);
    try {
      const run = await api.launch(selectedTask.id, selectedAgent);
      setRuns((current) => [...current, run]);
      setBusy(`${selectedAgent} active`);
    } catch (reason) { setError(reason instanceof Error ? reason.message : "Could not start agent"); setBusy("Start failed"); }
  }

  async function send() {
    if (!currentRun || !prompt.trim()) return;
    const message = prompt.trim();
    setPrompt("");
    try { await api.send(currentRun.id, message); setBusy("Agent working…"); }
    catch (reason) { setError(reason instanceof Error ? reason.message : "Could not send prompt"); }
  }

  async function verify() {
    setBusy("Verifying…");
    try { setVerification(await api.verify(["ctest", "--test-dir", "build"])); setBusy("Ready"); }
    catch (reason) { setError(reason instanceof Error ? reason.message : "Verification failed"); setBusy("Verification failed"); }
  }

  return <Layout
    repository={repository} tasks={tasks} selectedTask={selectedTask} onSelectTask={setSelectedTask}
    taskPrompt={taskPrompt} onTaskPrompt={setTaskPrompt} onCreateTask={createTask}
    agents={agents} selectedAgent={selectedAgent} onAgentChange={setSelectedAgent} onLaunch={launch}
    run={currentRun} events={currentEvents} prompt={prompt} onPrompt={setPrompt} onSend={send} busy={busy}
    drawer={drawer} onDrawer={setDrawer} diff={diff} verification={verification} onVerify={verify} error={error} />;
}
